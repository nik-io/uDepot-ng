#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "udepot/buffer.h"
#include "udepot/coro.h"
#include "udepot/directory.h"
#include "udepot/hash_entry.h"
#include "udepot/rcu.h"

#include "city.h"

namespace udepot {

// On-disk KV entry layout (unchanged from uDepot):
//   [u16 key_size][u32 val_size][u64 timestamp][key][value][u16 crc16]
// Total: 16 + key_size + val_size bytes.
struct __attribute__((packed)) KvHeader {
    uint16_t key_size;
    uint32_t val_size;
    uint64_t timestamp;
};

struct __attribute__((packed)) KvSuffix {
    uint16_t crc16;
};

static_assert(sizeof(KvHeader) == 14);
static_assert(sizeof(KvSuffix) == 2);

inline constexpr size_t kKvOverhead = sizeof(KvHeader) + sizeof(KvSuffix);

struct StoreConfig {
    const char* path = nullptr;
    size_t size = 0;
    uint32_t grain_size = 512;
    uint32_t initial_tables = 2;
    uint32_t index_bits = 10;
};

// High-performance KV store, parameterized on the I/O backend.
//
// Uses RCU-protected directory for lock-free reads, salsa for grain
// allocation, and the IoBackend for storage I/O.
template <typename IO>
class UDepot {
public:
    UDepot();
    ~UDepot();

    UDepot(const UDepot&) = delete;
    UDepot& operator=(const UDepot&) = delete;

    int open(const StoreConfig& config);
    void close();

    CoroTask<int> put(std::span<const uint8_t> key,
                      std::span<const uint8_t> val);

    CoroTask<int> get(std::span<const uint8_t> key,
                      uint8_t* val_out, size_t val_buf_size,
                      size_t* val_size_out);

    CoroTask<int> del(std::span<const uint8_t> key);

    CoroTask<int> exists(std::span<const uint8_t> key,
                         size_t* val_size_out);

    // Convenience overloads for string keys/values.
    CoroTask<int> put(std::string_view key, std::string_view val) {
        return put(
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(key.data()), key.size()),
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(val.data()), val.size()));
    }

    CoroTask<int> get(std::string_view key, uint8_t* val_out,
                      size_t val_buf_size, size_t* val_size_out) {
        return get(
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(key.data()), key.size()),
            val_out, val_buf_size, val_size_out);
    }

    CoroTask<int> del(std::string_view key) {
        return del(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(key.data()), key.size()));
    }

    CoroTask<int> exists(std::string_view key, size_t* val_size_out) {
        return exists(
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(key.data()), key.size()),
            val_size_out);
    }

    uint64_t hash_key(std::span<const uint8_t> key) const {
        return CityHash64(reinterpret_cast<const char*>(key.data()),
                          key.size());
    }

    Directory& directory() { return *directory_; }
    const Directory& directory() const { return *directory_; }
    Rcu& rcu() { return rcu_; }
    IO& io() { return io_; }
    uint32_t grain_size() const { return grain_size_; }

private:
    Rcu rcu_;
    IO io_;
    Directory* directory_ = nullptr;
    Rcu::Token rcu_token_{};
    uint32_t grain_size_ = 512;
    uint64_t total_grains_ = 0;

    // Simple bump allocator for v0. Salsa integration comes next.
    std::atomic<uint64_t> next_grain_{0};

    uint64_t allocate_grains(uint64_t count);
    void invalidate_grains(uint64_t grain, uint64_t count);

    static uint16_t compute_crc16(const KvHeader& hdr,
                                  std::span<const uint8_t> key,
                                  std::span<const uint8_t> val);

    size_t kv_total_bytes(size_t key_size, size_t val_size) const {
        return sizeof(KvHeader) + key_size + val_size + sizeof(KvSuffix);
    }

    uint64_t kv_total_grains(size_t key_size, size_t val_size) const {
        size_t bytes = kv_total_bytes(key_size, val_size);
        return (bytes + grain_size_ - 1) / grain_size_;
    }

    off_t grain_to_offset(uint64_t grain) const {
        return static_cast<off_t>(grain) * grain_size_;
    }

    // Read the on-disk header at a given PBA and verify the key matches.
    // Returns 0 if the key matches, ENOENT if it doesn't.
    CoroTask<int> verify_key_at_pba(uint64_t pba, uint16_t kv_grains,
                                    std::span<const uint8_t> key,
                                    KvHeader* hdr_out);
};

}  // namespace udepot
