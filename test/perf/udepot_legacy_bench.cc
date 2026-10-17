// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "kv.hh"
#include "trt/uapi/trt.hh"
#include "trt_backends/trt_aio.hh"
#include "trt_backends/trt_uring.hh"
#include "uDepot/backend.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-factory.hh"
#include "uDepot/udepot-lsa.hh"
#if defined(UDEPOT_TRT_SPDK)
#include "trt_backends/trt_spdk.hh"
#include <rte_lcore.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

static constexpr uint64_t kPrime = 0x9E3779B97F4A7C15ULL;

struct BenchConfig {
    uint64_t ops = 10000;
    uint32_t val_size = 1024;
    uint64_t seed = 42;
    uint32_t grain_size = 512;
    size_t store_size = 1077936129;
    const char* file = "/dev/shm/udepot-legacy-bench.store";
    int threads = 1;
    // posix (KV_UDEPOT_SALSA, plain threads), or a TRT backend: aio, uring,
    // spdk (KV_UDEPOT_SALSA_TRT_AIO/_URING/_SPDK), run as udepot-test runs
    // them.
    std::string backend = "posix";
    // TRT tasks per phase (--qd), udepot-test's --trt-ntasks: each takes a
    // contiguous slice of the ops, one at a time.
    uint32_t qd = 1;
};

static double now_secs() {
    auto t = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

struct ThreadResult {
    double put_secs = 0;
    double get_secs = 0;
    double exists_secs = 0;
    double del_secs = 0;
    int errors = 0;
};

static ThreadResult run_thread(KV* kv, const BenchConfig& cfg,
                               int thread_id) {
    ThreadResult result;
    uint64_t thread_seed = cfg.seed + thread_id * 1000000ULL;

    kv->thread_local_entry();

    std::vector<uint8_t> val(cfg.val_size, 0);
    uint8_t keyb[32] = {};

    // PUT
    double t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (thread_seed + i) * kPrime;
        uint64_t valu = key * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));
        std::memcpy(val.data(), &valu, sizeof(valu));

        int rc = static_cast<int>(kv->put(
            reinterpret_cast<const char*>(keyb), key_size,
            reinterpret_cast<const char*>(val.data()), cfg.val_size
        ).run_sync());
        if (rc != 0) {
            fprintf(stderr, "put failed: thread=%d i=%lu rc=%d\n",
                    thread_id, static_cast<unsigned long>(i), rc);
            ++result.errors;
            kv->thread_local_exit();
            return result;
        }
    }
    result.put_secs = now_secs() - t0;

    // GET
    std::vector<char> val_out(cfg.val_size);
    t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (thread_seed + i) * kPrime;
        uint64_t valu = key * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));

        size_t val_size_read = 0;
        size_t val_size_total = 0;
        int rc = static_cast<int>(kv->get(
            reinterpret_cast<const char*>(keyb), key_size,
            val_out.data(), val_out.size(),
            val_size_read, val_size_total
        ).run_sync());
        if (rc != 0) {
            fprintf(stderr, "get failed: thread=%d i=%lu rc=%d\n",
                    thread_id, static_cast<unsigned long>(i), rc);
            ++result.errors;
            kv->thread_local_exit();
            return result;
        }

        if (cfg.val_size >= sizeof(uint64_t)) {
            uint64_t val_ret;
            std::memcpy(&val_ret, val_out.data(), sizeof(val_ret));
            if (val_ret != valu) {
                fprintf(stderr, "value mismatch: thread=%d i=%lu\n",
                        thread_id, static_cast<unsigned long>(i));
                ++result.errors;
                kv->thread_local_exit();
                return result;
            }
        }
    }
    result.get_secs = now_secs() - t0;

    // EXISTS
    t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (thread_seed + i) * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));

        size_t val_sz = 0;
        int rc = static_cast<int>(kv->exists(
            reinterpret_cast<const char*>(keyb), key_size, val_sz
        ).run_sync());
        if (rc != 0) {
            fprintf(stderr, "exists failed: thread=%d i=%lu rc=%d\n",
                    thread_id, static_cast<unsigned long>(i), rc);
            ++result.errors;
            kv->thread_local_exit();
            return result;
        }
    }
    result.exists_secs = now_secs() - t0;

    // DEL
    t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (thread_seed + i) * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));

        int rc = static_cast<int>(kv->del(
            reinterpret_cast<const char*>(keyb), key_size
        ).run_sync());
        if (rc != 0) {
            fprintf(stderr, "del failed: thread=%d i=%lu rc=%d\n",
                    thread_id, static_cast<unsigned long>(i), rc);
            ++result.errors;
            kv->thread_local_exit();
            return result;
        }
    }
    result.del_secs = now_secs() - t0;

    kv->thread_local_exit();
    return result;
}

// ── TRT backends ───────────────────────────────────────────────────────────
// As udepot-test's t_main_bench and do_trt_run: one TRT scheduler, whose
// main task opens the store; each phase spawns cfg.qd tasks, each a
// contiguous slice of the ops done one at a time, and is timed from the
// spawn until every task has finished. udepot-test times only PUT and GET
// on TRT; EXISTS and DEL are timed the same way here.

enum { kPut, kGet, kExists, kDel, kPhases };

struct TrtRun {
    const BenchConfig* cfg = nullptr;
    udepot::KV_conf* conf = nullptr;
    double secs[kPhases] = {};
    int errors = 0;
    int init_rc = 0;
};

template <typename RT>
struct TrtWorkerArg {
    udepot::uDepotSalsa<RT>* kv;
    TrtRun* run;
    int phase;
    uint64_t start, end;
};

template <typename RT>
static trt::CoroTask trt_worker(void* arg_) {
    auto* a = static_cast<TrtWorkerArg<RT>*>(arg_);
    const BenchConfig& cfg = *a->run->cfg;
    std::vector<char> val(cfg.val_size, 0);
    char keyb[32] = {};
    for (uint64_t i = a->start; i < a->end && !a->run->errors; ++i) {
        const uint64_t key = (cfg.seed + i) * kPrime;
        const uint64_t valu = key * kPrime;
        const uint64_t key_size = 8 + (key % 24);
        std::memcpy(keyb, &key, sizeof(key));
        int rc = 0;
        size_t val_size = 0, val_size_read = 0;
        switch (a->phase) {
            case kPut:
                std::memcpy(val.data(), &valu, sizeof(valu));
                rc = (int)(co_await a->kv->put(keyb, key_size, val.data(),
                                               cfg.val_size));
                break;
            case kGet:
                rc = (int)(co_await a->kv->get(keyb, key_size, val.data(),
                                               cfg.val_size, val_size_read,
                                               val_size));
                if (rc == 0 && cfg.val_size >= sizeof(valu) &&
                    std::memcmp(val.data(), &valu, sizeof(valu)) != 0)
                    rc = EBADMSG;
                break;
            case kExists:
                rc = (int)(co_await a->kv->exists(keyb, key_size, val_size));
                break;
            default:
                rc = (int)(co_await a->kv->del(keyb, key_size));
                break;
        }
        if (rc != 0) {
            fprintf(stderr, "phase %d failed: i=%lu rc=%d\n", a->phase,
                    static_cast<unsigned long>(i), rc);
            ++a->run->errors;
        }
    }
    co_return 0;
}

template <typename RT>
static trt::CoroTask trt_main(void* arg_) {
    auto* run = static_cast<TrtRun*>(arg_);
    const BenchConfig& cfg = *run->cfg;
    KV* kv = udepot::KV_factory::KV_new(*run->conf);
    if (!kv || (run->init_rc = kv->init()) != 0) {
        if (!kv) run->init_rc = ENOMEM;
        delete kv;
        trt::T::set_exit_all();
        co_return 0;
    }
    auto* skv = static_cast<udepot::uDepotSalsa<RT>*>(kv);
    const uint64_t ntasks = std::max<uint32_t>(cfg.qd, 1);
    for (int phase = 0; phase < kPhases && !run->errors; ++phase) {
        std::vector<TrtWorkerArg<RT>> args(ntasks);
        trt::Task::List tl;
        for (uint64_t t = 0; t < ntasks; ++t) {
            // udepot-test's workload_part: the last task takes the rest.
            const uint64_t len = cfg.ops / ntasks;
            const uint64_t start = t * len;
            const uint64_t end = t == ntasks - 1 ? cfg.ops : start + len;
            args[t] = {skv, run, phase, start, end};
            trt::Task* task = trt::T::alloc_task(trt_worker<RT>, &args[t],
                                                 nullptr, false);
            tl.push_back(*task);
        }
        const double t0 = now_secs();
        co_await trt::T::spawn_many(tl);
        for (uint64_t t = 0; t < ntasks; ++t) co_await trt::T::task_wait();
        run->secs[phase] = now_secs() - t0;
    }
    kv->shutdown();
    delete kv;
    trt::T::set_exit_all();
    co_return 0;
}

template <typename RT>
static int run_trt(const BenchConfig& cfg, udepot::KV_conf& conf) {
    if (cfg.threads != 1) {
        fprintf(stderr, "TRT backends: one thread only\n");
        return 1;
    }
    TrtRun run;
    run.cfg = &cfg;
    run.conf = &conf;
#if defined(UDEPOT_TRT_SPDK)
    if constexpr (std::is_same_v<RT, udepot::RuntimeTrtSpdk>) {
        // As udepot-test: the scheduler runs on an RTE worker lcore.
        RT::IO::global_init();
        trt::RteController c;
        unsigned lcore = rte_get_next_lcore(-1, 1, 0);
        if (lcore >= RTE_MAX_LCORE) {
            fprintf(stderr, "no RTE worker lcore\n");
            return 1;
        }
        c.spawn_scheduler(trt_main<RT>, &run, trt::TaskType::TASK, lcore);
        c.wait_for_all();
    } else
#endif
    {
        trt::Controller c;
        c.spawn_scheduler(trt_main<RT>, &run, trt::TaskType::TASK);
        c.wait_for_all();
    }
    if (run.init_rc != 0) {
        fprintf(stderr, "init failed: %d\n", run.init_rc);
        return 1;
    }
    if (run.errors) return 1;
    static const char* names[kPhases] = {"PUT", "GET", "EXIST", "DEL"};
    for (int phase = 0; phase < kPhases; ++phase)
        printf("%ss Aggregate time=%lfs Mops/sec=%lf\n", names[phase],
               run.secs[phase], cfg.ops / (run.secs[phase] * 1e6));
    return 0;
}

static int run_bench(const BenchConfig& cfg) {
    uint64_t segment_size = (1ULL << 29) / cfg.grain_size + 2;
    udepot::KV_conf conf(
        cfg.file,
        cfg.store_size,
        true,
        cfg.grain_size,
        segment_size);
    conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA;
    if (cfg.backend == "aio")
        conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA_TRT_AIO;
    else if (cfg.backend == "uring")
        conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA_TRT_URING;
    else if (cfg.backend == "spdk")
        conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA_TRT_SPDK;
    else if (cfg.backend != "posix") {
        fprintf(stderr, "Unknown backend: %s\n", cfg.backend.c_str());
        return 1;
    }
    conf.validate_and_sanitize_parameters();  // as udepot-test
    if (cfg.backend == "aio") return run_trt<udepot::RuntimeTrt>(cfg, conf);
    if (cfg.backend == "uring")
        return run_trt<udepot::RuntimeTrtUring>(cfg, conf);
    if (cfg.backend == "spdk") {
#if defined(UDEPOT_TRT_SPDK)
        return run_trt<udepot::RuntimeTrtSpdk>(cfg, conf);
#else
        fprintf(stderr, "legacy uDepot built without SPDK\n");
        return 1;
#endif
    }

    KV* kv = udepot::KV_factory::KV_new(conf);
    if (!kv) {
        fprintf(stderr, "KV_new failed\n");
        return 1;
    }

    int rc = kv->init();
    if (rc != 0) {
        fprintf(stderr, "init failed: %d\n", rc);
        delete kv;
        return 1;
    }

    uint64_t total_ops = cfg.ops * static_cast<uint64_t>(cfg.threads);

    if (cfg.threads == 1) {
        auto r = run_thread(kv, cfg, 0);
        if (r.errors) { kv->shutdown(); delete kv; return 1; }

        printf("PUTs Aggregate time=%lfs Mops/sec=%lf\n",
               r.put_secs, cfg.ops / (r.put_secs * 1e6));
        printf("GETs Aggregate time=%lfs Mops/sec=%lf\n",
               r.get_secs, cfg.ops / (r.get_secs * 1e6));
        printf("EXISTs Aggregate time=%lfs Mops/sec=%lf\n",
               r.exists_secs, cfg.ops / (r.exists_secs * 1e6));
        printf("DELs Aggregate time=%lfs Mops/sec=%lf\n",
               r.del_secs, cfg.ops / (r.del_secs * 1e6));
    } else {
        std::vector<ThreadResult> results(cfg.threads);
        std::vector<std::thread> threads;

        for (int t = 0; t < cfg.threads; ++t) {
            threads.emplace_back([&, t] {
                results[t] = run_thread(kv, cfg, t);
            });
        }
        for (auto& th : threads) th.join();

        int total_errors = 0;
        double max_put = 0, max_get = 0, max_exists = 0, max_del = 0;
        for (int t = 0; t < cfg.threads; ++t) {
            total_errors += results[t].errors;
            if (results[t].put_secs > max_put) max_put = results[t].put_secs;
            if (results[t].get_secs > max_get) max_get = results[t].get_secs;
            if (results[t].exists_secs > max_exists)
                max_exists = results[t].exists_secs;
            if (results[t].del_secs > max_del) max_del = results[t].del_secs;
        }
        if (total_errors) { kv->shutdown(); delete kv; return 1; }

        printf("PUTs Aggregate time=%lfs Mops/sec=%lf threads=%d\n",
               max_put, total_ops / (max_put * 1e6), cfg.threads);
        printf("GETs Aggregate time=%lfs Mops/sec=%lf threads=%d\n",
               max_get, total_ops / (max_get * 1e6), cfg.threads);
        printf("EXISTs Aggregate time=%lfs Mops/sec=%lf threads=%d\n",
               max_exists, total_ops / (max_exists * 1e6), cfg.threads);
        printf("DELs Aggregate time=%lfs Mops/sec=%lf threads=%d\n",
               max_del, total_ops / (max_del * 1e6), cfg.threads);
    }

    kv->shutdown();
    delete kv;
    return 0;
}

static void usage() {
    fprintf(stderr,
        "Usage: udepot_legacy_bench [options]\n"
        "  -w <ops>       Number of operations per phase per thread (default: 10000)\n"
        "  -f <file>      Store file path (default: /dev/shm/udepot-legacy-bench.store)\n"
        "  --size <bytes> Store size (default: 1077936129)\n"
        "  --grain-size <bytes> Grain size (default: 512)\n"
        "  --val-size <bytes>   Value size (default: 1024)\n"
        "  --seed <n>     RNG seed (default: 42)\n"
        "  --threads <n>  Number of concurrent threads (default: 1)\n"
        "  --backend <b>  posix, aio, uring or spdk (default: posix); the\n"
        "                 last three on TRT, as udepot-test runs them\n"
        "  --qd <n>       TRT tasks per phase, each a slice of the ops\n"
        "                 (udepot-test's --trt-ntasks; default: 1)\n");
}

int main(int argc, char* argv[]) {
    BenchConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-w" && i + 1 < argc) {
            cfg.ops = std::stoull(argv[++i]);
        } else if (arg == "-f" && i + 1 < argc) {
            cfg.file = argv[++i];
        } else if (arg == "--size" && i + 1 < argc) {
            cfg.store_size = std::stoull(argv[++i]);
        } else if (arg == "--grain-size" && i + 1 < argc) {
            cfg.grain_size = std::stoul(argv[++i]);
        } else if (arg == "--val-size" && i + 1 < argc) {
            cfg.val_size = std::stoul(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            cfg.seed = std::stoull(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            cfg.threads = std::stoi(argv[++i]);
        } else if (arg == "--backend" && i + 1 < argc) {
            cfg.backend = argv[++i];
        } else if (arg == "--qd" && i + 1 < argc) {
            cfg.qd = std::stoul(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            usage();
            return 1;
        }
    }

    int rc = run_bench(cfg);
    return rc;
}
