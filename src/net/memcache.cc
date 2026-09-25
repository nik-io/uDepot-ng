// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "net/memcache.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "udepot/coro.h"
#include "udepot/io/net.h"
#include "udepot/store.h"

#include "udepot/io/aio.h"
#include "udepot/io/posix.h"
#ifdef UDEPOT_BUILD_URING
#include "udepot/io/uring.h"
#endif

namespace udepot {

// ─────────────────────────────────────────────────────────────────────────────
// Protocol constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr std::string_view kCRLF = "\r\n";
static constexpr std::string_view kStored = "STORED\r\n";
static constexpr std::string_view kNotStored = "NOT_STORED\r\n";
static constexpr std::string_view kNotFound = "NOT_FOUND\r\n";
static constexpr std::string_view kDeleted = "DELETED\r\n";
static constexpr std::string_view kEnd = "END\r\n";
static constexpr std::string_view kError = "ERROR\r\n";
static constexpr std::string_view kClientError = "CLIENT_ERROR ";
static constexpr std::string_view kServerError = "SERVER_ERROR ";
static constexpr std::string_view kExists = "EXISTS\r\n";

// ─────────────────────────────────────────────────────────────────────────────
// Request types
// ─────────────────────────────────────────────────────────────────────────────
enum class ReqType {
    kGet, kGets, kSet, kAdd, kReplace, kAppend, kPrepend,
    kCas, kIncr, kDecr, kDelete, kStats, kQuit, kVersion,
    kFlushAll, kUnknown
};

static ReqType parse_command(std::string_view cmd) {
    if (cmd == "get")       return ReqType::kGet;
    if (cmd == "gets")      return ReqType::kGets;
    if (cmd == "set")       return ReqType::kSet;
    if (cmd == "add")       return ReqType::kAdd;
    if (cmd == "replace")   return ReqType::kReplace;
    if (cmd == "append")    return ReqType::kAppend;
    if (cmd == "prepend")   return ReqType::kPrepend;
    if (cmd == "cas")       return ReqType::kCas;
    if (cmd == "incr")      return ReqType::kIncr;
    if (cmd == "decr")      return ReqType::kDecr;
    if (cmd == "delete")    return ReqType::kDelete;
    if (cmd == "stats")     return ReqType::kStats;
    if (cmd == "quit")      return ReqType::kQuit;
    if (cmd == "version")   return ReqType::kVersion;
    if (cmd == "flush_all") return ReqType::kFlushAll;
    return ReqType::kUnknown;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tokenizer
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> tokens;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;
        size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

// ─────────────────────────────────────────────────────────────────────────────
// Number parsing
// ─────────────────────────────────────────────────────────────────────────────
static bool parse_u32(std::string_view s, uint32_t& out) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

static bool parse_u64(std::string_view s, uint64_t& out) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// Expiry: matches uDepot/memcached semantics.
//   0          = never expire
//   <= 30 days = relative delta from now
//   > 30 days  = absolute Unix timestamp
// ─────────────────────────────────────────────────────────────────────────────
static int64_t resolve_expiry(uint64_t client_exptime) {
    if (client_exptime == 0) return 0;
    auto now = static_cast<int64_t>(std::time(nullptr));
    if (static_cast<int64_t>(client_exptime) <=
        MemcacheServer<UDepot<PosixIO>>::kRealtimeMaxdelta) {
        return now + static_cast<int64_t>(client_exptime);
    }
    return static_cast<int64_t>(client_exptime);
}

static bool is_expired(int64_t expiry) {
    if (expiry == 0) return false;
    return std::time(nullptr) >= expiry;
}

// ─────────────────────────────────────────────────────────────────────────────
// Metadata encoding: [user_value][expiry: int64_t][flags: uint32_t]
// ─────────────────────────────────────────────────────────────────────────────
static constexpr size_t kMetaSize = 12;

static void encode_metadata(uint8_t* dest, int64_t expiry, uint32_t flags) {
    std::memcpy(dest, &expiry, sizeof(expiry));
    std::memcpy(dest + sizeof(expiry), &flags, sizeof(flags));
}

static void decode_metadata(const uint8_t* src, size_t total_size,
                            int64_t& expiry, uint32_t& flags) {
    const uint8_t* meta = src + total_size - kMetaSize;
    std::memcpy(&expiry, meta, sizeof(expiry));
    std::memcpy(&flags, meta + sizeof(expiry), sizeof(flags));
}

// ─────────────────────────────────────────────────────────────────────────────
// MemcacheServer — connection handler coroutine
//
// Each connection runs in its own thread with its own EpollState. The handler
// reads commands from the socket, dispatches to the store, and writes
// responses back.
// ─────────────────────────────────────────────────────────────────────────────

// RecvBuffer: growable receive buffer for reading line-oriented protocol.
struct RecvBuffer {
    std::vector<char> data;
    size_t used = 0;
    size_t parsed = 0;

    RecvBuffer() : data(4096) {}

    // Find \r\n starting from parsed position.
    // Returns offset of \r or npos if not found.
    size_t find_crlf() const {
        for (size_t i = parsed; i + 1 < used; ++i) {
            if (data[i] == '\r' && data[i + 1] == '\n')
                return i;
        }
        return std::string_view::npos;
    }

    // Compact: move unparsed data to front.
    void compact() {
        if (parsed > 0) {
            size_t remaining = used - parsed;
            if (remaining > 0)
                std::memmove(data.data(), data.data() + parsed, remaining);
            used = remaining;
            parsed = 0;
        }
    }

    // Ensure we have room for at least n more bytes.
    void ensure_space(size_t n) {
        if (used + n > data.size())
            data.resize(std::max(data.size() * 2, used + n));
    }

    char* write_ptr() { return data.data() + used; }
    size_t write_avail() const { return data.size() - used; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Handler helpers — templated on the Store type so they can call store.put etc.
// ─────────────────────────────────────────────────────────────────────────────

template <typename Store>
static CoroTask<int> handle_store(
    Store& store, Connection& conn, ReqType req,
    std::string_view key, uint32_t flags, uint64_t exptime,
    const uint8_t* value_data, size_t value_len,
    bool noreply, std::atomic<uint64_t>& bytes_stored) {

    int64_t expiry = resolve_expiry(exptime);

    // Build stored value: [user_data][expiry][flags]
    std::vector<uint8_t> stored(value_len + kMetaSize);
    std::memcpy(stored.data(), value_data, value_len);
    encode_metadata(stored.data() + value_len, expiry, flags);

    auto key_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(key.data()), key.size());
    auto val_span = std::span<const uint8_t>(stored.data(), stored.size());

    int rc = -1;

    if (req == ReqType::kSet) {
        rc = co_await store.put(key_span, val_span);
    } else if (req == ReqType::kAdd) {
        // ADD: store only if key does NOT exist.
        size_t existing_size = 0;
        int exists_rc = co_await store.exists(key_span, &existing_size);
        if (exists_rc == 0) {
            if (!noreply) co_await conn.send_full(
                kNotStored.data(), kNotStored.size(), 0);
            co_return 0;
        }
        rc = co_await store.put(key_span, val_span);
    } else if (req == ReqType::kReplace) {
        // REPLACE: store only if key DOES exist.
        size_t existing_size = 0;
        int exists_rc = co_await store.exists(key_span, &existing_size);
        if (exists_rc != 0) {
            if (!noreply) co_await conn.send_full(
                kNotStored.data(), kNotStored.size(), 0);
            co_return 0;
        }
        rc = co_await store.put(key_span, val_span);
    } else if (req == ReqType::kAppend || req == ReqType::kPrepend) {
        // APPEND/PREPEND: get existing value, merge, put back.
        std::vector<uint8_t> existing(kMetaSize + 1024 * 1024);
        size_t existing_size = 0;
        int get_rc = co_await store.get(key_span, existing.data(),
                                        existing.size(), &existing_size);
        if (get_rc != 0) {
            if (!noreply) co_await conn.send_full(
                kNotStored.data(), kNotStored.size(), 0);
            co_return 0;
        }

        // Extract existing metadata.
        if (existing_size < kMetaSize) {
            if (!noreply) co_await conn.send_full(
                kServerError.data(), kServerError.size(), 0);
            co_return 0;
        }
        int64_t old_expiry;
        uint32_t old_flags;
        decode_metadata(existing.data(), existing_size, old_expiry, old_flags);
        size_t old_data_len = existing_size - kMetaSize;

        // Build merged value.
        std::vector<uint8_t> merged(old_data_len + value_len + kMetaSize);
        if (req == ReqType::kAppend) {
            std::memcpy(merged.data(), existing.data(), old_data_len);
            std::memcpy(merged.data() + old_data_len, value_data, value_len);
        } else {
            std::memcpy(merged.data(), value_data, value_len);
            std::memcpy(merged.data() + value_len, existing.data(),
                        old_data_len);
        }
        encode_metadata(merged.data() + old_data_len + value_len,
                        expiry, flags);

        auto merged_span = std::span<const uint8_t>(
            merged.data(), merged.size());
        rc = co_await store.put(key_span, merged_span);
    }

    if (!noreply) {
        if (rc == 0) {
            co_await conn.send_full(kStored.data(), kStored.size(), 0);
            bytes_stored.fetch_add(value_len, std::memory_order_relaxed);
        } else {
            co_await conn.send_full(kNotStored.data(), kNotStored.size(), 0);
        }
    } else if (rc == 0) {
        bytes_stored.fetch_add(value_len, std::memory_order_relaxed);
    }
    co_return 0;
}

template <typename Store>
static CoroTask<int> handle_get(
    Store& store, Connection& conn,
    const std::vector<std::string_view>& keys) {

    for (auto& key : keys) {
        auto key_span = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(key.data()), key.size());

        std::vector<uint8_t> val_buf(kMetaSize + 1024 * 1024);
        size_t val_size = 0;
        int rc = co_await store.get(key_span, val_buf.data(),
                                    val_buf.size(), &val_size);
        if (rc != 0 || val_size < kMetaSize)
            continue;

        int64_t expiry;
        uint32_t flags;
        decode_metadata(val_buf.data(), val_size, expiry, flags);

        if (is_expired(expiry)) {
            co_await store.del(key_span);
            continue;
        }

        size_t data_len = val_size - kMetaSize;

        // FORMAT: VALUE <key> <flags> <bytes>\r\n<data>\r\n
        char header[512];
        int hlen = snprintf(header, sizeof(header), "VALUE %.*s %u %zu\r\n",
                            static_cast<int>(key.size()), key.data(),
                            flags, data_len);

        co_await conn.send_full(header, static_cast<size_t>(hlen), 0);
        if (data_len > 0)
            co_await conn.send_full(val_buf.data(), data_len, 0);
        co_await conn.send_full(kCRLF.data(), kCRLF.size(), 0);
    }
    co_await conn.send_full(kEnd.data(), kEnd.size(), 0);
    co_return 0;
}

template <typename Store>
static CoroTask<int> handle_delete(
    Store& store, Connection& conn,
    std::string_view key, bool noreply) {

    auto key_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(key.data()), key.size());

    int rc = co_await store.del(key_span);
    if (!noreply) {
        if (rc == 0)
            co_await conn.send_full(kDeleted.data(), kDeleted.size(), 0);
        else
            co_await conn.send_full(kNotFound.data(), kNotFound.size(), 0);
    }
    co_return 0;
}

template <typename Store>
static CoroTask<int> handle_arithmetic(
    Store& store, Connection& conn,
    std::string_view key, uint64_t delta, bool is_incr, bool noreply) {

    auto key_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(key.data()), key.size());

    std::vector<uint8_t> val_buf(kMetaSize + 128);
    size_t val_size = 0;
    int rc = co_await store.get(key_span, val_buf.data(),
                                val_buf.size(), &val_size);
    if (rc != 0 || val_size < kMetaSize) {
        if (!noreply)
            co_await conn.send_full(kNotFound.data(), kNotFound.size(), 0);
        co_return 0;
    }

    int64_t expiry;
    uint32_t flags;
    decode_metadata(val_buf.data(), val_size, expiry, flags);

    if (is_expired(expiry)) {
        co_await store.del(key_span);
        if (!noreply)
            co_await conn.send_full(kNotFound.data(), kNotFound.size(), 0);
        co_return 0;
    }

    size_t data_len = val_size - kMetaSize;
    std::string_view old_val(reinterpret_cast<char*>(val_buf.data()), data_len);

    uint64_t num = 0;
    auto r = std::from_chars(old_val.data(), old_val.data() + old_val.size(),
                             num);
    if (r.ec != std::errc{}) {
        if (!noreply) {
            static constexpr std::string_view msg =
                "CLIENT_ERROR cannot increment or decrement non-numeric value\r\n";
            co_await conn.send_full(msg.data(), msg.size(), 0);
        }
        co_return 0;
    }

    if (is_incr)
        num += delta;
    else
        num = (delta > num) ? 0 : num - delta;

    char new_val_str[32];
    auto [ptr, ec] = std::to_chars(new_val_str, new_val_str + sizeof(new_val_str), num);
    size_t new_data_len = static_cast<size_t>(ptr - new_val_str);

    // Build new stored value with same flags/expiry.
    std::vector<uint8_t> new_stored(new_data_len + kMetaSize);
    std::memcpy(new_stored.data(), new_val_str, new_data_len);
    encode_metadata(new_stored.data() + new_data_len, expiry, flags);

    auto val_span = std::span<const uint8_t>(new_stored.data(), new_stored.size());
    rc = co_await store.put(key_span, val_span);

    if (!noreply) {
        if (rc == 0) {
            // Reply with the new value.
            char reply[64];
            int rlen = snprintf(reply, sizeof(reply), "%.*s\r\n",
                                static_cast<int>(new_data_len), new_val_str);
            co_await conn.send_full(reply, static_cast<size_t>(rlen), 0);
        } else {
            co_await conn.send_full(kServerError.data(), kServerError.size(), 0);
        }
    }
    co_return 0;
}

template <typename Store>
static CoroTask<int> handle_stats(
    Connection& conn, const std::atomic<uint64_t>& bytes_stored) {

    char buf[128];
    int len = snprintf(buf, sizeof(buf), "STAT bytes %lu\r\n",
                       static_cast<unsigned long>(
                           bytes_stored.load(std::memory_order_relaxed)));
    co_await conn.send_full(buf, static_cast<size_t>(len), 0);
    co_await conn.send_full(kEnd.data(), kEnd.size(), 0);
    co_return 0;
}

static CoroTask<int> handle_version(Connection& conn) {
    static constexpr std::string_view ver = "VERSION udepot-ng 0.1.0\r\n";
    co_await conn.send_full(ver.data(), ver.size(), 0);
    co_return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection handler: reads commands, dispatches to handlers.
// ─────────────────────────────────────────────────────────────────────────────

template <typename Store>
static CoroTask<int> serve_connection(
    Store& store, EpollState& /*es*/, Connection& conn,
    std::atomic<bool>& running, std::atomic<uint64_t>& bytes_stored) {

    RecvBuffer buf;

    while (running.load(std::memory_order_relaxed)) {
        // Read a command line (terminated by \r\n).
        size_t crlf_pos;
        while ((crlf_pos = buf.find_crlf()) == std::string_view::npos) {
            buf.compact();
            buf.ensure_space(1024);
            ssize_t n = co_await conn.recv(buf.write_ptr(),
                                           buf.write_avail(), 0);
            if (n <= 0) co_return 0;  // Connection closed.
            buf.used += static_cast<size_t>(n);
        }

        std::string_view line(buf.data.data() + buf.parsed,
                              crlf_pos - buf.parsed);
        buf.parsed = crlf_pos + 2;  // Skip past \r\n.

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        // Lowercase the command.
        std::string cmd_str(tokens[0]);
        for (char& c : cmd_str) c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(c)));
        ReqType req = parse_command(cmd_str);

        if (req == ReqType::kQuit) {
            co_return 0;
        }

        if (req == ReqType::kVersion) {
            co_await handle_version(conn);
            continue;
        }

        if (req == ReqType::kStats) {
            co_await handle_stats<Store>(conn, bytes_stored);
            continue;
        }

        if (req == ReqType::kGet || req == ReqType::kGets) {
            // GET <key>*\r\n
            std::vector<std::string_view> keys(tokens.begin() + 1,
                                               tokens.end());
            if (keys.empty()) {
                co_await conn.send_full(kError.data(), kError.size(), 0);
                continue;
            }
            // Copy keys since they reference the recv buffer which may
            // be compacted.
            std::vector<std::string> owned_keys;
            owned_keys.reserve(keys.size());
            for (auto& k : keys) owned_keys.emplace_back(k);
            std::vector<std::string_view> key_views;
            key_views.reserve(owned_keys.size());
            for (auto& k : owned_keys) key_views.push_back(k);

            co_await handle_get<Store>(store, conn, key_views);
            continue;
        }

        if (req == ReqType::kSet || req == ReqType::kAdd ||
            req == ReqType::kReplace || req == ReqType::kAppend ||
            req == ReqType::kPrepend) {
            // <command> <key> <flags> <exptime> <bytes> [noreply]\r\n
            // <data block>\r\n
            if (tokens.size() < 5) {
                co_await conn.send_full(kError.data(), kError.size(), 0);
                continue;
            }

            std::string key_owned(tokens[1]);
            if (key_owned.size() > MemcacheServer<Store>::kMaxKeyLen) {
                static constexpr std::string_view msg =
                    "CLIENT_ERROR bad command line format\r\n";
                co_await conn.send_full(msg.data(), msg.size(), 0);
                continue;
            }

            uint32_t flags = 0;
            uint64_t exptime = 0;
            uint32_t nbytes = 0;
            if (!parse_u32(tokens[2], flags) ||
                !parse_u64(tokens[3], exptime) ||
                !parse_u32(tokens[4], nbytes)) {
                co_await conn.send_full(kError.data(), kError.size(), 0);
                continue;
            }

            if (nbytes > MemcacheServer<Store>::kMaxValueLen) {
                static constexpr std::string_view msg =
                    "SERVER_ERROR object too large for cache\r\n";
                co_await conn.send_full(msg.data(), msg.size(), 0);
                continue;
            }

            bool noreply = (tokens.size() >= 6 && tokens[5] == "noreply");

            // Read the data block: exactly nbytes + \r\n.
            size_t need = nbytes + 2;  // data + \r\n
            size_t have = buf.used - buf.parsed;
            while (have < need) {
                buf.compact();
                buf.ensure_space(need - have + 1);
                ssize_t n = co_await conn.recv(buf.write_ptr(),
                                               buf.write_avail(), 0);
                if (n <= 0) co_return 0;
                buf.used += static_cast<size_t>(n);
                have = buf.used - buf.parsed;
            }

            const uint8_t* value_data =
                reinterpret_cast<const uint8_t*>(buf.data.data() + buf.parsed);
            buf.parsed += need;

            co_await handle_store<Store>(
                store, conn, req, key_owned, flags, exptime,
                value_data, nbytes, noreply, bytes_stored);
            continue;
        }

        if (req == ReqType::kDelete) {
            // delete <key> [noreply]\r\n
            if (tokens.size() < 2) {
                co_await conn.send_full(kError.data(), kError.size(), 0);
                continue;
            }
            std::string key_owned(tokens[1]);
            bool noreply = (tokens.size() >= 3 && tokens[2] == "noreply");
            co_await handle_delete<Store>(store, conn, key_owned, noreply);
            continue;
        }

        if (req == ReqType::kIncr || req == ReqType::kDecr) {
            // incr/decr <key> <value> [noreply]\r\n
            if (tokens.size() < 3) {
                co_await conn.send_full(kError.data(), kError.size(), 0);
                continue;
            }
            std::string key_owned(tokens[1]);
            uint64_t delta = 0;
            if (!parse_u64(tokens[2], delta)) {
                co_await conn.send_full(kError.data(), kError.size(), 0);
                continue;
            }
            bool noreply = (tokens.size() >= 4 && tokens[3] == "noreply");
            co_await handle_arithmetic<Store>(
                store, conn, key_owned, delta,
                req == ReqType::kIncr, noreply);
            continue;
        }

        // Unknown command.
        co_await conn.send_full(kError.data(), kError.size(), 0);
    }
    co_return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// MemcacheServer implementation
// ─────────────────────────────────────────────────────────────────────────────

template <typename Store>
MemcacheServer<Store>::~MemcacheServer() {
    stop();
}

template <typename Store>
int MemcacheServer<Store>::start(const Config& config) {
    if (running_.load(std::memory_order_relaxed))
        return -EALREADY;

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return -errno;

    int optval = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, config.bind_addr.c_str(), &addr.sin_addr) != 1) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -EINVAL;
    }
    addr.sin_port = htons(config.port);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -e;
    }

    // Retrieve the actual port (useful when port == 0).
    socklen_t alen = sizeof(addr);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
    port_ = ntohs(addr.sin_port);

    if (::listen(listen_fd_, config.backlog) < 0) {
        int e = errno;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -e;
    }

    running_.store(true, std::memory_order_release);
    accept_thread_ = std::thread(&MemcacheServer::accept_loop, this);
    return 0;
}

template <typename Store>
void MemcacheServer<Store>::stop() {
    if (!running_.load(std::memory_order_relaxed))
        return;

    running_.store(false, std::memory_order_release);

    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (accept_thread_.joinable())
        accept_thread_.join();

    for (auto& t : conn_threads_) {
        if (t.joinable()) t.join();
    }
    conn_threads_.clear();
}

template <typename Store>
void MemcacheServer<Store>::accept_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        struct sockaddr_in cli_addr{};
        socklen_t cli_len = sizeof(cli_addr);
        int fd = ::accept(listen_fd_,
                          reinterpret_cast<sockaddr*>(&cli_addr), &cli_len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;  // listen_fd closed or fatal error.
        }

        int optval = 1;
        setsockopt(fd, SOL_TCP, TCP_NODELAY, &optval, sizeof(optval));

        conn_threads_.emplace_back(
            &MemcacheServer::handle_connection, this, fd);
    }
}

template <typename Store>
void MemcacheServer<Store>::handle_connection(int fd) {
    EpollState es;
    if (es.init() != 0) {
        ::close(fd);
        return;
    }

    es.register_fd(fd, EPOLLIN | EPOLLOUT);
    Connection conn(es, fd);

    auto task = serve_connection<Store>(
        store_, es, conn, running_, bytes_stored_);
    task.run_sync();

    es.close_fd(fd);
    es.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Explicit template instantiations
// ─────────────────────────────────────────────────────────────────────────────
template class MemcacheServer<UDepot<PosixIO>>;
template class MemcacheServer<UDepot<AioIO>>;
#ifdef UDEPOT_BUILD_URING
template class MemcacheServer<UDepot<UringIO>>;
#endif

}  // namespace udepot
