#include "udepot/store.h"
#include "udepot/io/posix.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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
};

static double now_secs() {
    auto t = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

static int run_bench(const BenchConfig& cfg) {
    StoreConfig sc;
    sc.path = cfg.file;
    sc.size = cfg.store_size;
    sc.grain_size = cfg.grain_size;
    sc.initial_tables = 4;
    sc.index_bits = 14;

    UDepot<PosixIO> store;
    int rc = store.open(sc);
    if (rc != 0) {
        fprintf(stderr, "open failed: %d\n", rc);
        return 1;
    }

    std::vector<uint8_t> val(cfg.val_size, 0);
    uint8_t keyb[32] = {};

    // PUT phase
    double t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (cfg.seed + i) * kPrime;
        uint64_t valu = key * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));
        std::memcpy(val.data(), &valu, sizeof(valu));

        rc = store.put(
            std::span<const uint8_t>(keyb, key_size),
            std::span<const uint8_t>(val.data(), cfg.val_size)).run_sync();
        if (rc != 0) {
            fprintf(stderr, "put failed at i=%lu: %d\n",
                    static_cast<unsigned long>(i), rc);
            store.close();
            return 1;
        }
    }
    double put_secs = now_secs() - t0;
    printf("PUTs aggregate   time=%lfs Mops/sec=%lf ops=%lu\n",
           put_secs, cfg.ops / (put_secs * 1e6), cfg.ops);

    // GET phase
    std::vector<uint8_t> val_out(cfg.val_size);
    t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (cfg.seed + i) * kPrime;
        uint64_t valu = key * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));

        size_t val_size_read = 0;
        rc = store.get(
            std::span<const uint8_t>(keyb, key_size),
            val_out.data(), val_out.size(), &val_size_read).run_sync();
        if (rc != 0) {
            fprintf(stderr, "get failed at i=%lu: %d\n",
                    static_cast<unsigned long>(i), rc);
            store.close();
            return 1;
        }

        if (cfg.val_size >= sizeof(uint64_t)) {
            uint64_t val_ret;
            std::memcpy(&val_ret, val_out.data(), sizeof(val_ret));
            if (val_ret != valu) {
                fprintf(stderr, "value mismatch at i=%lu\n",
                        static_cast<unsigned long>(i));
                store.close();
                return 1;
            }
        }
    }
    double get_secs = now_secs() - t0;
    printf("GETs aggregate   time=%lfs Mops/sec=%lf ops=%lu\n",
           get_secs, cfg.ops / (get_secs * 1e6), cfg.ops);

    store.close();
    return 0;
}

static void usage() {
    fprintf(stderr,
        "Usage: udepot_ng_bench [options]\n"
        "  -w <ops>       Number of put/get operations (default: 10000)\n"
        "  -f <file>      Store file path (default: /dev/shm/udepot-ng-bench.store)\n"
        "  --size <bytes> Store size (default: 1077936129)\n"
        "  --grain-size <bytes> Grain size (default: 512)\n"
        "  --val-size <bytes>   Value size (default: 1024)\n"
        "  --seed <n>     RNG seed (default: 42)\n");
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
    int rc = run_bench(cfg);
    std::filesystem::remove(cfg.file);
    return rc;
}
