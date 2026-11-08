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
#include <optional>
#include <string_view>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
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
static constexpr int kSendFlags = MSG_NOSIGNAL;

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
        MemcacheServer<uDepot<PosixIO>>::kRealtimeMaxdelta) {
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
// Each connection is a coroutine on one of the server's workers. It reads
// commands from the socket, dispatches to the store, and writes responses
// back.
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

    // The condition each command puts on the key is checked by the store in
    // the same critical section as the write, so concurrent clients cannot
    // both pass it.
    if (req == ReqType::kSet) {
        rc = co_await store.put(key_span, val_span);
    } else if (req == ReqType::kAdd) {
        rc = co_await store.put(key_span, val_span, PutMode::kCreate);
    } else if (req == ReqType::kReplace) {
        rc = co_await store.put(key_span, val_span, PutMode::kReplace);
    } else if (req == ReqType::kAppend || req == ReqType::kPrepend) {
        // Read-modify-write: replace only the version that was read, and
        // start over if another client changed it in between.
        for (;;) {
            // The whole stored value, however long: its metadata is at
            // the end, where a copy into a fixed buffer would cut it off.
            GetBuffer got;
            uint64_t version = kAnyVersion;
            int get_rc = co_await store.get(key_span, &got, &version);
            if (get_rc != 0) {
                if (!noreply) co_await conn.send_full(
                    kNotStored.data(), kNotStored.size(), 0);
                co_return 0;
            }

            const uint8_t* existing = got.value().data();
            const size_t existing_size = got.value().size();
            // Extract existing metadata.
            if (existing_size < kMetaSize) {
                if (!noreply) co_await conn.send_full(
                    kServerError.data(), kServerError.size(), 0);
                co_return 0;
            }
            int64_t old_expiry;
            uint32_t old_flags;
            decode_metadata(existing, existing_size, old_expiry, old_flags);
            size_t old_data_len = existing_size - kMetaSize;
            if (old_data_len + value_len >
                MemcacheServer<Store>::kMaxValueLen) {
                rc = -E2BIG;  // as a set of that size would be refused
                break;
            }

            // Build merged value.
            std::vector<uint8_t> merged(old_data_len + value_len + kMetaSize);
            if (req == ReqType::kAppend) {
                std::memcpy(merged.data(), existing, old_data_len);
                std::memcpy(merged.data() + old_data_len, value_data,
                            value_len);
            } else {
                std::memcpy(merged.data(), value_data, value_len);
                std::memcpy(merged.data() + value_len, existing,
                            old_data_len);
            }
            encode_metadata(merged.data() + old_data_len + value_len,
                            expiry, flags);

            auto merged_span = std::span<const uint8_t>(
                merged.data(), merged.size());
            rc = co_await store.put(key_span, merged_span, PutMode::kReplace,
                                    version);
            if (rc != -ESTALE) break;
        }
    }

    if (!noreply) {
        if (rc == 0) {
            co_await conn.send_full(kStored.data(), kStored.size(), kSendFlags);
            bytes_stored.fetch_add(value_len, std::memory_order_relaxed);
        } else if (rc == -E2BIG) {
            static constexpr std::string_view msg =
                "SERVER_ERROR object too large for cache\r\n";
            co_await conn.send_full(msg.data(), msg.size(), kSendFlags);
        } else {
            co_await conn.send_full(kNotStored.data(), kNotStored.size(), kSendFlags);
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

        // Zero copy, and the whole value: its metadata is at the end.
        GetBuffer got;
        uint64_t version = kAnyVersion;
        int rc = co_await store.get(key_span, &got, &version);
        const uint8_t* val = got.value().data();
        const size_t val_size = got.value().size();
        if (rc != 0 || val_size < kMetaSize)
            continue;

        int64_t expiry;
        uint32_t flags;
        decode_metadata(val, val_size, expiry, flags);

        if (is_expired(expiry)) {
            // Only the expired value: a set that landed since must survive.
            (void)co_await store.del(key_span, version);
            continue;
        }

        size_t data_len = val_size - kMetaSize;

        // FORMAT: VALUE <key> <flags> <bytes>\r\n<data>\r\n
        char header[512];
        int hlen = snprintf(header, sizeof(header), "VALUE %.*s %u %zu\r\n",
                            static_cast<int>(key.size()), key.data(),
                            flags, data_len);

        co_await conn.send_full(header, static_cast<size_t>(hlen), kSendFlags);
        if (data_len > 0)
            co_await conn.send_full(val, data_len, kSendFlags);
        co_await conn.send_full(kCRLF.data(), kCRLF.size(), kSendFlags);
    }
    co_await conn.send_full(kEnd.data(), kEnd.size(), kSendFlags);
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
            co_await conn.send_full(kDeleted.data(), kDeleted.size(), kSendFlags);
        else
            co_await conn.send_full(kNotFound.data(), kNotFound.size(), kSendFlags);
    }
    co_return 0;
}

template <typename Store>
static CoroTask<int> handle_arithmetic(
    Store& store, Connection& conn,
    std::string_view key, uint64_t delta, bool is_incr, bool noreply) {

    auto key_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(key.data()), key.size());

    char new_val_str[32];
    size_t new_data_len = 0;
    int rc;
    // Read-modify-write: replace only the version that was read, and start
    // over if another client changed it in between.
    for (;;) {
        // The whole value, as in handle_get.
        GetBuffer got;
        uint64_t version = kAnyVersion;
        rc = co_await store.get(key_span, &got, &version);
        const uint8_t* val = got.value().data();
        const size_t val_size = got.value().size();
        if (rc != 0 || val_size < kMetaSize) {
            if (!noreply)
                co_await conn.send_full(kNotFound.data(), kNotFound.size(),
                                        kSendFlags);
            co_return 0;
        }

        int64_t expiry;
        uint32_t flags;
        decode_metadata(val, val_size, expiry, flags);

        if (is_expired(expiry)) {
            (void)co_await store.del(key_span, version);
            if (!noreply)
                co_await conn.send_full(kNotFound.data(), kNotFound.size(),
                                        kSendFlags);
            co_return 0;
        }

        size_t data_len = val_size - kMetaSize;
        std::string_view old_val(reinterpret_cast<const char*>(val),
                                 data_len);

        uint64_t num = 0;
        auto r = std::from_chars(old_val.data(),
                                 old_val.data() + old_val.size(), num);
        if (r.ec != std::errc{}) {
            if (!noreply) {
                static constexpr std::string_view msg =
                    "CLIENT_ERROR cannot increment or decrement non-numeric "
                    "value\r\n";
                co_await conn.send_full(msg.data(), msg.size(), kSendFlags);
            }
            co_return 0;
        }

        if (is_incr)
            num += delta;
        else
            num = (delta > num) ? 0 : num - delta;

        auto [ptr, ec] = std::to_chars(new_val_str,
                                       new_val_str + sizeof(new_val_str), num);
        new_data_len = static_cast<size_t>(ptr - new_val_str);

        // Build new stored value with same flags/expiry.
        std::vector<uint8_t> new_stored(new_data_len + kMetaSize);
        std::memcpy(new_stored.data(), new_val_str, new_data_len);
        encode_metadata(new_stored.data() + new_data_len, expiry, flags);

        auto val_span = std::span<const uint8_t>(new_stored.data(),
                                                 new_stored.size());
        rc = co_await store.put(key_span, val_span, PutMode::kReplace,
                                version);
        if (rc != -ESTALE) break;
    }

    if (rc == -ENOENT) {  // deleted between the get and the put
        if (!noreply)
            co_await conn.send_full(kNotFound.data(), kNotFound.size(),
                                    kSendFlags);
        co_return 0;
    }

    if (!noreply) {
        if (rc == 0) {
            // Reply with the new value.
            char reply[64];
            int rlen = snprintf(reply, sizeof(reply), "%.*s\r\n",
                                static_cast<int>(new_data_len), new_val_str);
            co_await conn.send_full(reply, static_cast<size_t>(rlen), kSendFlags);
        } else {
            co_await conn.send_full(kServerError.data(), kServerError.size(), kSendFlags);
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
    co_await conn.send_full(buf, static_cast<size_t>(len), kSendFlags);
    co_await conn.send_full(kEnd.data(), kEnd.size(), kSendFlags);
    co_return 0;
}

static CoroTask<int> handle_version(Connection& conn) {
    static constexpr std::string_view ver = "VERSION udepot-ng 0.1.0\r\n";
    co_await conn.send_full(ver.data(), ver.size(), kSendFlags);
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
                co_await conn.send_full(kError.data(), kError.size(), kSendFlags);
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
                co_await conn.send_full(kError.data(), kError.size(), kSendFlags);
                continue;
            }

            std::string key_owned(tokens[1]);
            if (key_owned.size() > MemcacheServer<Store>::kMaxKeyLen) {
                static constexpr std::string_view msg =
                    "CLIENT_ERROR bad command line format\r\n";
                co_await conn.send_full(msg.data(), msg.size(), kSendFlags);
                continue;
            }

            uint32_t flags = 0;
            uint64_t exptime = 0;
            uint32_t nbytes = 0;
            if (!parse_u32(tokens[2], flags) ||
                !parse_u64(tokens[3], exptime) ||
                !parse_u32(tokens[4], nbytes)) {
                co_await conn.send_full(kError.data(), kError.size(), kSendFlags);
                continue;
            }

            if (nbytes > MemcacheServer<Store>::kMaxValueLen) {
                static constexpr std::string_view msg =
                    "SERVER_ERROR object too large for cache\r\n";
                co_await conn.send_full(msg.data(), msg.size(), kSendFlags);
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
                co_await conn.send_full(kError.data(), kError.size(), kSendFlags);
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
                co_await conn.send_full(kError.data(), kError.size(), kSendFlags);
                continue;
            }
            std::string key_owned(tokens[1]);
            uint64_t delta = 0;
            if (!parse_u64(tokens[2], delta)) {
                co_await conn.send_full(kError.data(), kError.size(), kSendFlags);
                continue;
            }
            bool noreply = (tokens.size() >= 4 && tokens[3] == "noreply");
            co_await handle_arithmetic<Store>(
                store, conn, key_owned, delta,
                req == ReqType::kIncr, noreply);
            continue;
        }

        // Unknown command.
        co_await conn.send_full(kError.data(), kError.size(), kSendFlags);
    }
    co_return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// MemcacheServer implementation


// A worker: one thread running one EpollState's poller, which polls the
// sockets and this thread's store I/O completions side by side, as one of
// uDepot's TRT scheduler threads ran its epoll and I/O pollers (and the
// connection tasks) -- trt-memcache.cc:122-127 spawns the epoll poller on
// every scheduler thread, task_mc_accept (:238-270) spawns each accepted
// connection as a task on one of them (SpawnPolicy::Distribute).
//
// Everything a worker owns (conns, acceptor) is touched only by its poller
// thread, which starts, resumes and reclaims the connection coroutines, so
// it needs no lock. Two exceptions in time: start() creates the worker and
// starts its acceptor before the poller has anything to act on (the poller
// never reads `acceptor`), and stop_workers() destroys it after the poller
// is joined. The other exceptions are the atomics: a connection coroutine
// may finish on another thread (the store's space waker resumes a waiting
// operation, and the coroutine runs on from there until its next socket
// wait), so finishing touches nothing but the socket (under the epoll's own
// lock), conn_fds_ (its existing mutex) and the `finished` counter.
//
// Hand-off of an accepted fd: the accept thread sends the fd number down
// this worker's socketpair, whose read end the worker's epoll watches, and
// the worker's acceptor coroutine starts the connection on the worker's own
// thread. No userspace lock is involved (the kernel queues the message), and
// the accept thread chooses the worker round robin. The alternative, every
// worker accepting on the shared listen fd, would spread connections by
// whichever worker wins the race rather than evenly; legacy distributes
// round robin.
template <typename Store>
struct MemcacheServer<Store>::Worker {
    struct Conn {
        Connection conn;
        std::optional<CoroTask<int>> task;
        Conn(EpollState& es, int fd) : conn(es, fd) {}
    };

    unsigned index = 0;  // for messages and the test hooks
    EpollState es;
    int rx = -1;  // read end of the hand-off socketpair: worker thread
    int tx = -1;  // write end: accept thread; stop() closes it to end the worker
    std::optional<CoroTask<int>> acceptor;
    std::atomic<bool> acceptor_done{false};
    std::unordered_map<Conn*, std::unique_ptr<Conn>> conns;  // poller thread
    std::atomic<size_t> finished{0};  // finished, not yet reclaimed: any thread
    std::atomic<size_t> nconns{0};    // conns.size(), readable anywhere

    ~Worker() {
        if (rx >= 0) ::close(rx);
        if (tx >= 0) ::close(tx);
    }

    // A connection: serves it, then closes its fd. The fd leaves conn_fds_
    // before it is closed, so stop() can never shutdown() a number that has
    // been reused.
    CoroTask<int> run_connection(MemcacheServer& srv, Conn& c) {
        const int fd = c.conn.fd();
        int rc = co_await serve_connection<Store>(
            srv.store_, es, c.conn, srv.running_, srv.bytes_stored_);
        {
            std::lock_guard<std::mutex> lk(srv.conn_fds_mu_);
            auto it = std::find(srv.conn_fds_.begin(), srv.conn_fds_.end(), fd);
            if (it != srv.conn_fds_.end()) srv.conn_fds_.erase(it);
        }
        es.close_fd(fd);
        if (auto hook = MemcacheTestHooks::connection_finished_test_hook.load(
                std::memory_order_relaxed))
            hook();
        // The last thing this body does: after it the poller may reclaim
        // the frame once the task reports completion.
        finished.fetch_add(1, std::memory_order_release);
        co_return rc;
    }

    // Starts a connection on this thread (the poller's).
    void spawn(MemcacheServer& srv, int fd) {
        auto c = std::make_unique<Conn>(es, fd);
        Conn* p = c.get();
        conns.emplace(p, std::move(c));
        nconns.store(conns.size(), std::memory_order_relaxed);
        es.register_fd(fd, EPOLLIN | EPOLLOUT);
        // Eager start: runs to the connection's first suspension here.
        p->task.emplace(run_connection(srv, *p));
    }

    // Receives the fds the accept thread hands over until it closes its end
    // (after the last one: a socketpair delivers what is queued before EOF).
    CoroTask<int> accept_handoffs(MemcacheServer& srv) {
        for (;;) {
            int fd = -1;
            ssize_t n = co_await es.recv(rx, &fd, sizeof(fd), 0);
            const bool have_fd = n == static_cast<ssize_t>(sizeof(fd));
            if (auto hook = MemcacheTestHooks::handoff_recv_test_hook.load(
                    std::memory_order_relaxed)) {
                if (int e = hook(index); e != 0) {
                    n = -1;
                    errno = e;
                }
            }
            if (n == static_cast<ssize_t>(sizeof(fd))) {
                spawn(srv, fd);
                continue;
            }
            if (n < 0) {
                std::fprintf(stderr,
                             "udepot: memcache worker %u: hand-off socket "
                             "failed (%s); this worker takes no more "
                             "connections and closes those still queued for "
                             "it\n", index, std::strerror(errno));
                if (have_fd) {  // received, but failed: close it as the rest
                    std::lock_guard<std::mutex> lk(srv.conn_fds_mu_);
                    auto it = std::find(srv.conn_fds_.begin(),
                                        srv.conn_fds_.end(), fd);
                    if (it != srv.conn_fds_.end()) srv.conn_fds_.erase(it);
                    ::close(fd);
                }
                while (::recv(rx, &fd, sizeof(fd), MSG_DONTWAIT) ==
                       static_cast<ssize_t>(sizeof(fd))) {
                    std::lock_guard<std::mutex> lk(srv.conn_fds_mu_);
                    auto it = std::find(srv.conn_fds_.begin(),
                                        srv.conn_fds_.end(), fd);
                    if (it != srv.conn_fds_.end()) srv.conn_fds_.erase(it);
                    ::close(fd);
                }
            }
            break;  // 0: the accept side is closed and everything queued done.
        }
        es.close_fd(rx);
        rx = -1;
        acceptor_done.store(true, std::memory_order_release);
        co_return 0;
    }

    // The poller's hook, at the top of every iteration: reclaims finished
    // connections and ends the poller once there will be no more.
    bool poll() {
        if (finished.load(std::memory_order_acquire) != 0) {
            size_t reclaimed = 0;
            for (auto it = conns.begin(); it != conns.end();) {
                // await_ready() is the task's completion test: it holds
                // once the coroutine's final suspend ran, the last thing
                // it does on whichever thread finished it.
                if (it->second->task && it->second->task->await_ready()) {
                    it = conns.erase(it);
                    ++reclaimed;
                } else {
                    ++it;
                }
            }
            finished.fetch_sub(reclaimed, std::memory_order_relaxed);
            nconns.store(conns.size(), std::memory_order_relaxed);
        }
        return !(acceptor_done.load(std::memory_order_acquire) &&
                 conns.empty());
    }
};

template <typename Store>
MemcacheServer<Store>::MemcacheServer(Store& store) : store_(store) {}

template <typename Store>
MemcacheServer<Store>::~MemcacheServer() {
    stop();
}

template <typename Store>
size_t MemcacheServer<Store>::connections() const noexcept {
    size_t n = 0;
    for (const auto& w : workers_) n += w->nconns.load(std::memory_order_relaxed);
    return n;
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
        port_ = 0;
        return -e;
    }

    unsigned nworkers = config.workers;
    if (nworkers == 0) nworkers = std::thread::hardware_concurrency();
    if (nworkers == 0) nworkers = 1;  // hardware_concurrency() may not know.

    int err = 0;
    for (unsigned i = 0; i < nworkers && err == 0; ++i) {
        auto w = std::make_unique<Worker>();
        w->index = i;
        int sv[2];
        int sp_errno = 0;
        if (auto hook = MemcacheTestHooks::socketpair_test_hook.load(
                std::memory_order_relaxed))
            sp_errno = hook(i);
        if (sp_errno == 0 &&
            socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0)
            sp_errno = errno;
        if (sp_errno != 0) {
            err = -sp_errno;
            std::fprintf(stderr,
                         "udepot: memcache worker %u: socketpair: %s; the "
                         "server does not start\n",
                         i, std::strerror(sp_errno));
            break;
        }
        w->rx = sv[0];
        w->tx = sv[1];
        Worker* wp = w.get();
        w->es.set_poll_hook([wp] { return wp->poll(); });
        workers_.push_back(std::move(w));
        if ((err = wp->es.init()) != 0) {
            std::fprintf(stderr,
                         "udepot: memcache worker %u: epoll: %s; the server "
                         "does not start\n", i, std::strerror(-err));
            break;
        }
        wp->es.register_fd(wp->rx, EPOLLIN);
        wp->acceptor.emplace(wp->accept_handoffs(*this));
    }
    if (err != 0) {
        stop_workers();
        ::close(listen_fd_);
        listen_fd_ = -1;
        port_ = 0;
        return err;
    }

    next_worker_ = 0;
    handoff_failures_ = 0;
    refused_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    accept_thread_ = std::thread(&MemcacheServer::accept_loop, this);
    return 0;
}

// Ends the workers: closing the hand-off socket's write end tells each
// acceptor, after the last queued fd, to stop; its poller then exits when
// its connections are done. The accept thread must be gone (it sends on
// that socket).
template <typename Store>
void MemcacheServer<Store>::stop_workers() {
    for (auto& w : workers_) {
        if (w->tx >= 0) {
            ::close(w->tx);
            w->tx = -1;
        }
    }
    // Every connection ends: the caller shut their sockets down.
    for (auto& w : workers_) {
        w->es.join_poller();
        w->es.stop();
    }
    workers_.clear();
}

template <typename Store>
void MemcacheServer<Store>::stop() {
    if (!running_.load(std::memory_order_relaxed))
        return;

    running_.store(false, std::memory_order_release);

    // shutdown() wakes the accept thread; the fd is closed only once that
    // thread is gone, since its number could otherwise be reused while
    // accept_loop is still about to accept() on it.
    if (listen_fd_ >= 0)
        shutdown(listen_fd_, SHUT_RDWR);

    if (accept_thread_.joinable())
        accept_thread_.join();

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    // Force-close all tracked connection fds so handler coroutines unblock.
    // A connection removes its fd from conn_fds_ before it closes it, so
    // this never touches a number that has been reused.
    {
        std::lock_guard<std::mutex> lk(conn_fds_mu_);
        for (int fd : conn_fds_)
            shutdown(fd, SHUT_RDWR);
    }

    stop_workers();
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

        {
            std::lock_guard<std::mutex> lk(conn_fds_mu_);
            conn_fds_.push_back(fd);
        }

        // Round robin over the workers. The hand-off never blocks: a worker
        // that stopped draining its socket (its poller stalled or died)
        // must not hold up the accept thread, and with it every other
        // worker's connections and stop(). A full queue passes the
        // connection to the next worker; if none can take it, it is closed.
        bool handed = false;
        for (size_t tries = 0; tries < workers_.size(); ++tries) {
            Worker& w = *workers_[next_worker_];
            next_worker_ = (next_worker_ + 1) % workers_.size();
            if (::send(w.tx, &fd, sizeof(fd), MSG_NOSIGNAL | MSG_DONTWAIT) ==
                static_cast<ssize_t>(sizeof(fd))) {
                handed = true;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            // Broken, not just full. Said on the 1st, 2nd, 4th, ... so a
            // dead worker cannot flood the log.
            const int e = errno;
            if (const uint64_t n = ++handoff_failures_; (n & (n - 1)) == 0)
                std::fprintf(stderr,
                             "udepot: memcache: could not hand a connection "
                             "to worker %u (%s); %llu such failures so far\n",
                             w.index, std::strerror(e),
                             static_cast<unsigned long long>(n));
        }
        if (!handed) {
            {
                std::lock_guard<std::mutex> lk(conn_fds_mu_);
                auto it = std::find(conn_fds_.begin(), conn_fds_.end(), fd);
                if (it != conn_fds_.end()) conn_fds_.erase(it);
            }
            ::close(fd);
            const uint64_t n =
                refused_.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0)
                std::fprintf(stderr,
                             "udepot: memcache: no worker could take a "
                             "connection (every hand-off queue is full or "
                             "broken); closed it, %llu so far\n",
                             static_cast<unsigned long long>(n));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Explicit template instantiations
// ─────────────────────────────────────────────────────────────────────────────
template class MemcacheServer<uDepot<PosixIO>>;
template class MemcacheServer<uDepot<AioIO>>;
#ifdef UDEPOT_BUILD_URING
template class MemcacheServer<uDepot<UringIO>>;
#endif

}  // namespace udepot
