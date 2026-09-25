#include "kv.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-factory.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
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

static int run_bench(const BenchConfig& cfg) {
    uint64_t segment_size = (1ULL << 29) / cfg.grain_size + 2;
    udepot::KV_conf conf(
        cfg.file,
        cfg.store_size,
        true,
        cfg.grain_size,
        segment_size);
    conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA;

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
        "  --threads <n>  Number of concurrent threads (default: 1)\n");
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
