#include "kv.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-factory.hh"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static constexpr uint64_t kPrime = 0x9E3779B97F4A7C15ULL;

struct BenchConfig {
    uint64_t ops = 10000;
    uint32_t val_size = 1024;
    uint64_t seed = 42;
    uint32_t grain_size = 512;
    size_t store_size = 1077936129;
    const char* file = "/dev/shm/udepot-legacy-bench.store";
};

static double now_secs() {
    auto t = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t.time_since_epoch()).count();
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

    kv->thread_local_entry();

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

        rc = static_cast<int>(kv->put(
            reinterpret_cast<const char*>(keyb), key_size,
            reinterpret_cast<const char*>(val.data()), cfg.val_size
        ).run_sync());
        if (rc != 0) {
            fprintf(stderr, "put failed at i=%lu: %d\n",
                    static_cast<unsigned long>(i), rc);
            kv->thread_local_exit();
            kv->shutdown();
            delete kv;
            return 1;
        }
    }
    double put_secs = now_secs() - t0;
    printf("PUTs Aggregate time=%lfs Mops/sec=%lf\n",
           put_secs, cfg.ops / (put_secs * 1e6));

    // GET phase
    std::vector<char> val_out(cfg.val_size);
    t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (cfg.seed + i) * kPrime;
        uint64_t valu = key * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));

        size_t val_size_read = 0;
        size_t val_size_total = 0;
        rc = static_cast<int>(kv->get(
            reinterpret_cast<const char*>(keyb), key_size,
            val_out.data(), val_out.size(),
            val_size_read, val_size_total
        ).run_sync());
        if (rc != 0) {
            fprintf(stderr, "get failed at i=%lu: %d\n",
                    static_cast<unsigned long>(i), rc);
            kv->thread_local_exit();
            kv->shutdown();
            delete kv;
            return 1;
        }

        if (cfg.val_size >= sizeof(uint64_t)) {
            uint64_t val_ret;
            std::memcpy(&val_ret, val_out.data(), sizeof(val_ret));
            if (val_ret != valu) {
                fprintf(stderr, "value mismatch at i=%lu\n",
                        static_cast<unsigned long>(i));
                kv->thread_local_exit();
                kv->shutdown();
                delete kv;
                return 1;
            }
        }
    }
    double get_secs = now_secs() - t0;
    printf("GETs Aggregate time=%lfs Mops/sec=%lf\n",
           get_secs, cfg.ops / (get_secs * 1e6));

    // EXISTS phase
    t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (cfg.seed + i) * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));

        size_t val_sz = 0;
        rc = static_cast<int>(kv->exists(
            reinterpret_cast<const char*>(keyb), key_size, val_sz
        ).run_sync());
        if (rc != 0) {
            fprintf(stderr, "exists failed at i=%lu: %d\n",
                    static_cast<unsigned long>(i), rc);
            kv->thread_local_exit();
            kv->shutdown();
            delete kv;
            return 1;
        }
    }
    double exists_secs = now_secs() - t0;
    printf("EXISTs Aggregate time=%lfs Mops/sec=%lf\n",
           exists_secs, cfg.ops / (exists_secs * 1e6));

    // DEL phase
    t0 = now_secs();
    for (uint64_t i = 0; i < cfg.ops; ++i) {
        uint64_t key = (cfg.seed + i) * kPrime;
        uint64_t key_size = 8 + (key % 24);

        std::memcpy(keyb, &key, sizeof(key));

        rc = static_cast<int>(kv->del(
            reinterpret_cast<const char*>(keyb), key_size
        ).run_sync());
        if (rc != 0) {
            fprintf(stderr, "del failed at i=%lu: %d\n",
                    static_cast<unsigned long>(i), rc);
            kv->thread_local_exit();
            kv->shutdown();
            delete kv;
            return 1;
        }
    }
    double del_secs = now_secs() - t0;
    printf("DELs Aggregate time=%lfs Mops/sec=%lf\n",
           del_secs, cfg.ops / (del_secs * 1e6));

    kv->thread_local_exit();
    kv->shutdown();
    delete kv;
    return 0;
}

static void usage() {
    fprintf(stderr,
        "Usage: udepot_legacy_bench [options]\n"
        "  -w <ops>       Number of operations per phase (default: 10000)\n"
        "  -f <file>      Store file path (default: /dev/shm/udepot-legacy-bench.store)\n"
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

    int rc = run_bench(cfg);
    return rc;
}
