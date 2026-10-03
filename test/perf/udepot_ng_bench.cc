// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/store.h"
#include "udepot/io/aio.h"
#include "udepot/io/posix.h"
#ifdef UDEPOT_BUILD_URING
#include "udepot/io/uring.h"
#endif
#ifdef UDEPOT_BUILD_SPDK
#include "udepot/io/spdk.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using udepot::AioIO;
using udepot::PosixIO;
using udepot::StoreConfig;
using udepot::UDepot;

static constexpr uint64_t kPrime = 0x9E3779B97F4A7C15ULL;

struct BenchConfig {
    uint64_t ops = 10000;
    uint32_t val_size = 1024;
    uint64_t seed = 42;
    uint32_t grain_size = 512;
    size_t store_size = 1077936129;
    const char* file = "/dev/shm/udepot-ng-bench.store";
    int threads = 1;
    std::string backend = "posix";
    // Values through the zero-copy interface (alloc_put_buffer + put, get
    // into a GetBuffer) instead of the copying one. The workload is
    // otherwise identical: the only difference is the value copies the
    // zero-copy path avoids.
    bool zero_copy = false;
    // Compare copy and zero copy in one process instead (--compare R):
    // R rounds, each a batch of `ops` copying operations and a batch of
    // `ops` zero-copy ones, in alternating order, against the same store.
    int compare_rounds = 0;
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

template <typename IO>
static ThreadResult run_thread(UDepot<IO>& store,
                               const BenchConfig& cfg,
                               int thread_id) {
    ThreadResult result;
    uint64_t thread_seed = cfg.seed + thread_id * 1000000ULL;

    std::vector<uint8_t> val(cfg.val_size, 0);
    uint8_t keyb[32] = {};

    // PUT
    double t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (thread_seed + i) * kPrime;
        uint64_t valu = key * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));

        int rc;
        if (cfg.zero_copy) {
            // The value is produced straight into the record buffer.
            auto pb = store.alloc_put_buffer(key_size, cfg.val_size);
            std::memcpy(pb.value().data(), &valu, sizeof(valu));
            rc = store.put(std::span<const uint8_t>(keyb, key_size), pb)
                     .run_sync();
        } else {
            std::memcpy(val.data(), &valu, sizeof(valu));
            rc = store.put(
                std::span<const uint8_t>(keyb, key_size),
                std::span<const uint8_t>(val.data(), cfg.val_size)).run_sync();
        }
        if (rc != 0) {
            fprintf(stderr, "put failed: thread=%d i=%lu rc=%d\n",
                    thread_id, static_cast<unsigned long>(i), rc);
            ++result.errors;
            return result;
        }
    }
    result.put_secs = now_secs() - t0;

    // GET
    std::vector<uint8_t> val_out(cfg.val_size);
    udepot::GetBuffer gb;
    t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (thread_seed + i) * kPrime;
        uint64_t valu = key * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));

        size_t val_size_read = 0;
        const uint8_t* got = val_out.data();
        int rc;
        if (cfg.zero_copy) {
            rc = store.get(std::span<const uint8_t>(keyb, key_size), &gb)
                     .run_sync();
            got = gb.value().data();
        } else {
            rc = store.get(
                std::span<const uint8_t>(keyb, key_size),
                val_out.data(), val_out.size(), &val_size_read).run_sync();
        }
        if (rc != 0) {
            fprintf(stderr, "get failed: thread=%d i=%lu rc=%d\n",
                    thread_id, static_cast<unsigned long>(i), rc);
            ++result.errors;
            return result;
        }

        if (cfg.val_size >= sizeof(uint64_t)) {
            uint64_t val_ret;
            std::memcpy(&val_ret, got, sizeof(val_ret));
            if (val_ret != valu) {
                fprintf(stderr, "value mismatch: thread=%d i=%lu\n",
                        thread_id, static_cast<unsigned long>(i));
                ++result.errors;
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
        int rc = store.exists(
            std::span<const uint8_t>(keyb, key_size), &val_sz).run_sync();
        if (rc != 0) {
            fprintf(stderr, "exists failed: thread=%d i=%lu rc=%d\n",
                    thread_id, static_cast<unsigned long>(i), rc);
            ++result.errors;
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

        int rc = store.del(
            std::span<const uint8_t>(keyb, key_size)).run_sync();
        if (rc != 0) {
            fprintf(stderr, "del failed: thread=%d i=%lu rc=%d\n",
                    thread_id, static_cast<unsigned long>(i), rc);
            ++result.errors;
            return result;
        }
    }
    result.del_secs = now_secs() - t0;

    return result;
}

static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    if (n == 0) return 0;
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// Copy against zero copy, batch by batch within one store. Separate runs
// of each compare two processes, and on a backend whose device is reached
// over a network (SPDK's NVMe-oF target) the drift between processes is
// far larger than the copy being measured; adjacent batches share it.
// Prints, per phase, each mode's median batch throughput and the median of
// the per-round ratios zero copy / copy: a round's two batches run back to
// back, so the ratio cancels what drifts (the target, the network, GC).
template <typename IO>
static int run_compare(UDepot<IO>& store, const BenchConfig& cfg) {
    const uint64_t batch = cfg.ops;
    std::vector<uint8_t> val(cfg.val_size, 0), val_out(cfg.val_size);
    udepot::GetBuffer gb;
    uint8_t keyb[32] = {};
    auto key_of = [&](uint64_t i, uint64_t* valu) {
        const uint64_t key = (cfg.seed + i) * kPrime;
        *valu = key * kPrime;
        std::memcpy(keyb, &key, sizeof(key));
        return std::span<const uint8_t>(keyb, 8 + key % 24);
    };
    auto put_batch = [&](uint64_t first, bool zc) -> double {
        const double t0 = now_secs();
        for (uint64_t i = first; i < first + batch; ++i) {
            uint64_t valu;
            auto key = key_of(i, &valu);
            int rc;
            if (zc) {
                auto pb = store.alloc_put_buffer(key.size(), cfg.val_size);
                std::memcpy(pb.value().data(), &valu, sizeof(valu));
                rc = store.put(key, pb).run_sync();
            } else {
                std::memcpy(val.data(), &valu, sizeof(valu));
                rc = store.put(key, std::span<const uint8_t>(val)).run_sync();
            }
            if (rc != 0) {
                fprintf(stderr, "put failed: i=%lu rc=%d\n",
                        static_cast<unsigned long>(i), rc);
                return -1;
            }
        }
        return now_secs() - t0;
    };
    auto get_batch = [&](uint64_t first, bool zc) -> double {
        const double t0 = now_secs();
        for (uint64_t i = first; i < first + batch; ++i) {
            uint64_t valu;
            auto key = key_of(i, &valu);
            const uint8_t* got = val_out.data();
            size_t n = 0;
            int rc;
            if (zc) {
                rc = store.get(key, &gb).run_sync();
                got = gb.value().data();
            } else {
                rc = store.get(key, val_out.data(), val_out.size(), &n)
                         .run_sync();
            }
            uint64_t val_ret = 0;
            if (rc == 0) std::memcpy(&val_ret, got, sizeof(val_ret));
            if (rc != 0 || val_ret != valu) {
                fprintf(stderr, "get failed: i=%lu rc=%d\n",
                        static_cast<unsigned long>(i), rc);
                return -1;
            }
        }
        return now_secs() - t0;
    };
    struct Phase {
        std::vector<double> copy, zc, ratio;
    };
    auto phase = [&](auto run_batch, Phase* ph) {
        uint64_t next = 0;
        for (int r = 0; r < cfg.compare_rounds; ++r) {
            double mops[2] = {0, 0};  // [copy, zero copy]
            for (int m = 0; m < 2; ++m) {
                const bool zero = (m == 0) == (r % 2 == 1);  // alternate
                const double secs = run_batch(next, zero);
                if (secs < 0) return false;
                next += batch;
                mops[zero] = batch / (secs * 1e6);
            }
            ph->copy.push_back(mops[0]);
            ph->zc.push_back(mops[1]);
            ph->ratio.push_back(mops[1] / mops[0]);
        }
        return true;
    };
    Phase put, get;
    if (!phase(put_batch, &put) || !phase(get_batch, &get)) return 1;
    for (auto [name, ph] : {std::pair{"PUT", &put}, std::pair{"GET", &get}})
        printf("CMP %s copy=%lf zero_copy=%lf delta=%+.2lf\n", name,
               median(ph->copy), median(ph->zc),
               (median(ph->ratio) - 1) * 100);
#ifdef UDEPOT_BUILD_SPDK
    if constexpr (std::is_same_v<IO, udepot::SpdkIO>)
        printf("BOUNCED %lu\n", static_cast<unsigned long>(
                                    udepot::SpdkIO::thread_bounce_count()));
#endif
    return 0;
}

template <typename IO>
static int run_bench(const BenchConfig& cfg) {
    StoreConfig sc;
    sc.path = cfg.file;
    sc.size = cfg.store_size;
    sc.grain_size = cfg.grain_size;
    sc.initial_tables = 4;
    sc.index_bits = 14;
    // Every run starts empty: a file is removed between runs, but an SPDK
    // namespace keeps the previous run's store.
    sc.force_destroy = true;

    UDepot<IO> store;
    int rc = store.open(sc);
    if (rc != 0) {
        fprintf(stderr, "open failed: %d\n", rc);
        return 1;
    }

    if (cfg.compare_rounds > 0) {
        rc = run_compare(store, cfg);
        store.close();
        return rc;
    }

    uint64_t total_ops = cfg.ops * static_cast<uint64_t>(cfg.threads);

    if (cfg.threads == 1) {
        auto r = run_thread(store, cfg, 0);
        if (r.errors) { store.close(); return 1; }

        printf("PUTs Aggregate time=%lfs Mops/sec=%lf\n",
               r.put_secs, cfg.ops / (r.put_secs * 1e6));
        printf("GETs Aggregate time=%lfs Mops/sec=%lf\n",
               r.get_secs, cfg.ops / (r.get_secs * 1e6));
        printf("EXISTs Aggregate time=%lfs Mops/sec=%lf\n",
               r.exists_secs, cfg.ops / (r.exists_secs * 1e6));
        printf("DELs Aggregate time=%lfs Mops/sec=%lf\n",
               r.del_secs, cfg.ops / (r.del_secs * 1e6));
#ifdef UDEPOT_BUILD_SPDK
        // I/Os SPDK copied through a bounce buffer because their buffer was
        // not DMA memory: zero copy hands the device the store's own.
        if constexpr (std::is_same_v<IO, udepot::SpdkIO>)
            printf("BOUNCED %lu\n", static_cast<unsigned long>(
                                        udepot::SpdkIO::thread_bounce_count()));
#endif
    } else {
        std::vector<ThreadResult> results(cfg.threads);
        std::vector<std::thread> threads;

        for (int t = 0; t < cfg.threads; ++t) {
            threads.emplace_back([&, t] {
                results[t] = run_thread(store, cfg, t);
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
        if (total_errors) { store.close(); return 1; }

        printf("PUTs Aggregate time=%lfs Mops/sec=%lf threads=%d\n",
               max_put, total_ops / (max_put * 1e6), cfg.threads);
        printf("GETs Aggregate time=%lfs Mops/sec=%lf threads=%d\n",
               max_get, total_ops / (max_get * 1e6), cfg.threads);
        printf("EXISTs Aggregate time=%lfs Mops/sec=%lf threads=%d\n",
               max_exists, total_ops / (max_exists * 1e6), cfg.threads);
        printf("DELs Aggregate time=%lfs Mops/sec=%lf threads=%d\n",
               max_del, total_ops / (max_del * 1e6), cfg.threads);
    }

    store.close();
    return 0;
}

static void usage() {
    fprintf(stderr,
        "Usage: udepot_ng_bench [options]\n"
        "  -w <ops>       Number of operations per phase per thread (default: 10000)\n"
        "  -f <file>      Store file path (default: /dev/shm/udepot-ng-bench.store)\n"
        "  --size <bytes> Store size (default: 1077936129)\n"
        "  --grain-size <bytes> Grain size (default: 512)\n"
        "  --val-size <bytes>   Value size (default: 1024)\n"
        "  --seed <n>     RNG seed (default: 42)\n"
        "  --threads <n>  Number of concurrent threads (default: 1)\n"
        "  --backend <b>  posix, aio, uring or spdk (default: posix); spdk\n"
        "                 uses the namespace UDEPOT_NVMEF names, whole\n"
        "  --zero-copy    Use the zero-copy put/get interface\n"
        "  --compare <r>  Copy vs zero copy in one store: r rounds of a\n"
        "                 batch of -w ops each way, order alternating\n");
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
        } else if (arg == "--zero-copy") {
            cfg.zero_copy = true;
        } else if (arg == "--compare" && i + 1 < argc) {
            cfg.compare_rounds = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            usage();
            return 1;
        }
    }

    std::filesystem::remove(cfg.file);
    int rc;
    if (cfg.backend == "posix") {
        rc = run_bench<PosixIO>(cfg);
    } else if (cfg.backend == "aio") {
        rc = run_bench<AioIO>(cfg);
#ifdef UDEPOT_BUILD_URING
    } else if (cfg.backend == "uring") {
        rc = run_bench<udepot::UringIO>(cfg);
#endif
#ifdef UDEPOT_BUILD_SPDK
    } else if (cfg.backend == "spdk") {
        if (udepot::SpdkIO::global_init() != 0) {
            fprintf(stderr, "SpdkIO::global_init failed\n");
            return 1;
        }
        BenchConfig spdk_cfg = cfg;
        spdk_cfg.file = "SPDK";
        spdk_cfg.store_size = 0;  // the whole namespace
        rc = run_bench<udepot::SpdkIO>(spdk_cfg);
        udepot::SpdkIO::global_shutdown();
        return rc;
#endif
    } else {
        fprintf(stderr, "Unknown backend: %s\n", cfg.backend.c_str());
        return 1;
    }
    std::filesystem::remove(cfg.file);
    return rc;
}
