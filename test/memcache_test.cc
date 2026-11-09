// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "net/memcache.h"
#include "udepot/io/net.h"
#include "udepot/store.h"
#include "udepot/io/aio.h"
#include "udepot/io/posix.h"
#ifdef UDEPOT_BUILD_URING
#include "udepot/io/uring.h"
#endif

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

using udepot::MemcacheServer;
using udepot::PosixIO;
using udepot::StoreConfig;
using udepot::uDepot;

static constexpr size_t kStoreSize = 4 * 1024 * 1024;

// ─────────────────────────────────────────────────────────────────────────────
// TCP client helper — blocking socket for sending memcache commands.
// ─────────────────────────────────────────────────────────────────────────────
class McClient {
public:
    McClient() = default;
    ~McClient() { close(); }

    McClient(const McClient&) = delete;
    McClient& operator=(const McClient&) = delete;

    bool connect(uint16_t port) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Set a recv timeout so tests don't hang.
        struct timeval tv{};
        tv.tv_sec = 5;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return true;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool send_cmd(std::string_view cmd) {
        auto* p = cmd.data();
        size_t rem = cmd.size();
        while (rem > 0) {
            ssize_t n = ::send(fd_, p, rem, 0);
            if (n <= 0) return false;
            p += n;
            rem -= static_cast<size_t>(n);
        }
        return true;
    }

    std::string recv_line() {
        std::string result;
        char c;
        while (true) {
            ssize_t n = ::recv(fd_, &c, 1, 0);
            if (n <= 0) break;
            result += c;
            if (result.size() >= 2 &&
                result[result.size() - 2] == '\r' &&
                result[result.size() - 1] == '\n') {
                result.resize(result.size() - 2);
                break;
            }
        }
        return result;
    }

    // Read until we get "END\r\n".
    std::string recv_until_end() {
        std::string result;
        while (true) {
            std::string line = recv_line();
            if (line.empty()) break;
            if (line == "END") break;
            result += line + "\n";
        }
        return result;
    }

    // Read a get response: VALUE line, data line, END line.
    // Returns the data, or empty string if not found.
    struct GetResult {
        std::string data;
        uint32_t flags = 0;
        bool found = false;
    };

    GetResult recv_get_response() {
        GetResult result;
        std::string line = recv_line();
        if (line.substr(0, 6) == "VALUE ") {
            // Parse: VALUE <key> <flags> <bytes>
            auto parts = split(line, ' ');
            if (parts.size() >= 4) {
                result.flags = static_cast<uint32_t>(std::stoul(parts[2]));
                size_t nbytes = std::stoull(parts[3]);
                result.data.resize(nbytes);
                recv_exact(result.data.data(), nbytes);
                recv_line();  // Consume trailing \r\n after data.
                result.found = true;
            }
            recv_line();  // END
        }
        return result;
    }

private:
    int fd_ = -1;

    void recv_exact(char* buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::recv(fd_, buf + got, len - got, 0);
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
    }

    static std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == delim) {
                if (i > start)
                    parts.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        return parts;
    }
};

namespace {

// Ends the process if the scope outlives `timeout`: a stop() that hangs
// would otherwise hang the test instead of failing it.
class Watchdog {
public:
    Watchdog(const char* what, std::chrono::seconds timeout)
        : thread_([this, what, timeout] {
              const auto deadline = std::chrono::steady_clock::now() + timeout;
              while (!done_) {
                  if (std::chrono::steady_clock::now() > deadline) {
                      std::fprintf(stderr, "FAIL: %s hung\n", what);
                      std::fflush(nullptr);
                      std::_Exit(1);
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
          }) {}
    ~Watchdog() {
        done_ = true;
        thread_.join();
    }

private:
    std::atomic<bool> done_{false};
    std::thread thread_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture
// ─────────────────────────────────────────────────────────────────────────────
class MemcacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("udepot_memcache_test_" + std::to_string(getpid()));
        StoreConfig config;
        config.path = path_.c_str();
        config.size = store_size();
        config.grain_size = grain_size();
        config.force_destroy = true;
        ASSERT_EQ(store_.open(config), 0);

        MemcacheServer<uDepot<PosixIO>>::Config mc_config;
        mc_config.bind_addr = "127.0.0.1";
        mc_config.port = 0;  // Ephemeral port.
        ASSERT_EQ(server_.start(mc_config), 0);
        port_ = server_.port();
        ASSERT_GT(port_, 0);

        // Give the accept thread time to be ready.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void TearDown() override {
        {
            Watchdog watchdog("stop()", std::chrono::seconds(30));
            server_.stop();
        }
        store_.close();
        std::filesystem::remove(path_);
    }

    virtual size_t store_size() const { return kStoreSize; }
    virtual uint32_t grain_size() const { return 512; }

    std::filesystem::path path_;
    uDepot<PosixIO> store_;
    MemcacheServer<uDepot<PosixIO>> server_{store_};
    uint16_t port_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(MemcacheTest, SetAndGet) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("set mykey 0 0 5\r\nhello\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("get mykey\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, "hello");
    EXPECT_EQ(result.flags, 0u);
}

TEST_F(MemcacheTest, SetWithFlags) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("set flagkey 42 0 3\r\nabc\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("get flagkey\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, "abc");
    EXPECT_EQ(result.flags, 42u);
}

TEST_F(MemcacheTest, GetNonexistentKey) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("get nosuchkey\r\n"));
    EXPECT_EQ(client.recv_line(), "END");
}

TEST_F(MemcacheTest, DeleteExistingKey) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("set delkey 0 0 3\r\nfoo\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("delete delkey\r\n"));
    EXPECT_EQ(client.recv_line(), "DELETED");

    ASSERT_TRUE(client.send_cmd("get delkey\r\n"));
    EXPECT_EQ(client.recv_line(), "END");
}

TEST_F(MemcacheTest, DeleteNonexistentKey) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("delete nokey\r\n"));
    EXPECT_EQ(client.recv_line(), "NOT_FOUND");
}

TEST_F(MemcacheTest, Increment) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("set counter 0 0 1\r\n5\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("incr counter 3\r\n"));
    EXPECT_EQ(client.recv_line(), "8");

    ASSERT_TRUE(client.send_cmd("get counter\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, "8");
}

TEST_F(MemcacheTest, Decrement) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("set counter 0 0 2\r\n10\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("decr counter 3\r\n"));
    EXPECT_EQ(client.recv_line(), "7");
}

TEST_F(MemcacheTest, DecrementBelowZeroClamps) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("set counter 0 0 1\r\n2\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("decr counter 10\r\n"));
    EXPECT_EQ(client.recv_line(), "0");
}

TEST_F(MemcacheTest, IncrNonexistentKey) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("incr nokey 1\r\n"));
    EXPECT_EQ(client.recv_line(), "NOT_FOUND");
}

TEST_F(MemcacheTest, Add) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    // ADD new key — should succeed.
    ASSERT_TRUE(client.send_cmd("add newkey 0 0 3\r\nbar\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    // ADD same key — should fail.
    ASSERT_TRUE(client.send_cmd("add newkey 0 0 3\r\nbaz\r\n"));
    EXPECT_EQ(client.recv_line(), "NOT_STORED");

    // Verify original value is intact.
    ASSERT_TRUE(client.send_cmd("get newkey\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, "bar");
}

TEST_F(MemcacheTest, Replace) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    // REPLACE nonexistent key — should fail.
    ASSERT_TRUE(client.send_cmd("replace nokey 0 0 3\r\nabc\r\n"));
    EXPECT_EQ(client.recv_line(), "NOT_STORED");

    // SET then REPLACE — should succeed.
    ASSERT_TRUE(client.send_cmd("set repkey 0 0 3\r\nold\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("replace repkey 0 0 3\r\nnew\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("get repkey\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, "new");
}

TEST_F(MemcacheTest, Append) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("set appkey 0 0 3\r\nfoo\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("append appkey 0 0 3\r\nbar\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("get appkey\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, "foobar");
}

TEST_F(MemcacheTest, Prepend) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("set prekey 0 0 3\r\nbar\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("prepend prekey 0 0 3\r\nfoo\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("get prekey\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, "foobar");
}

TEST_F(MemcacheTest, Version) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("version\r\n"));
    std::string line = client.recv_line();
    EXPECT_TRUE(line.find("VERSION") != std::string::npos);
    EXPECT_TRUE(line.find("udepot-ng") != std::string::npos);
}

TEST_F(MemcacheTest, Stats) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("stats\r\n"));
    std::string body = client.recv_until_end();
    EXPECT_TRUE(body.find("STAT bytes") != std::string::npos);
}

TEST_F(MemcacheTest, UnknownCommand) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("foobar\r\n"));
    EXPECT_EQ(client.recv_line(), "ERROR");
}

TEST_F(MemcacheTest, QuitClosesConnection) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("quit\r\n"));
    // After quit, the next recv should return empty (connection closed).
    std::string line = client.recv_line();
    EXPECT_TRUE(line.empty());
}

TEST_F(MemcacheTest, SetOverwritesExistingKey) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("set ow 0 0 5\r\nfirst\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("set ow 0 0 6\r\nsecond\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("get ow\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, "second");
}

TEST_F(MemcacheTest, MultipleKeysOnSameConnection) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    for (int i = 0; i < 50; ++i) {
        std::string key = "mk" + std::to_string(i);
        std::string val = "val" + std::to_string(i);
        std::string cmd = "set " + key + " 0 0 " +
                          std::to_string(val.size()) + "\r\n" + val + "\r\n";
        ASSERT_TRUE(client.send_cmd(cmd));
        EXPECT_EQ(client.recv_line(), "STORED") << "set i=" << i;
    }

    for (int i = 0; i < 50; ++i) {
        std::string key = "mk" + std::to_string(i);
        std::string expected = "val" + std::to_string(i);
        std::string cmd = "get " + key + "\r\n";
        ASSERT_TRUE(client.send_cmd(cmd));
        auto result = client.recv_get_response();
        EXPECT_TRUE(result.found) << "key=" << key;
        EXPECT_EQ(result.data, expected) << "key=" << key;
    }
}

TEST_F(MemcacheTest, MultipleConnections) {
    constexpr int kClients = 5;
    constexpr int kOpsPerClient = 20;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&, c] {
            McClient client;
            if (!client.connect(port_)) {
                errors.fetch_add(1);
                return;
            }

            for (int i = 0; i < kOpsPerClient; ++i) {
                std::string key = "c" + std::to_string(c) +
                                  "_k" + std::to_string(i);
                std::string val = "v" + std::to_string(c) +
                                  "_" + std::to_string(i);
                std::string cmd = "set " + key + " 0 0 " +
                                  std::to_string(val.size()) + "\r\n" +
                                  val + "\r\n";
                if (!client.send_cmd(cmd)) { errors.fetch_add(1); return; }
                std::string resp = client.recv_line();
                if (resp != "STORED") errors.fetch_add(1);
            }

            for (int i = 0; i < kOpsPerClient; ++i) {
                std::string key = "c" + std::to_string(c) +
                                  "_k" + std::to_string(i);
                std::string expected = "v" + std::to_string(c) +
                                       "_" + std::to_string(i);
                std::string cmd = "get " + key + "\r\n";
                if (!client.send_cmd(cmd)) { errors.fetch_add(1); return; }
                auto result = client.recv_get_response();
                if (!result.found || result.data != expected)
                    errors.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(errors.load(), 0);
}

TEST_F(MemcacheTest, Noreply) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    // SET with noreply — no response expected.
    ASSERT_TRUE(client.send_cmd("set nrkey 0 0 3 noreply\r\nfoo\r\n"));

    // Verify the value was stored by doing a GET.
    ASSERT_TRUE(client.send_cmd("get nrkey\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, "foo");
}

TEST_F(MemcacheTest, LargeValue) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    std::string large_val(8192, 'X');
    std::string cmd = "set bigval 0 0 " +
                      std::to_string(large_val.size()) + "\r\n" +
                      large_val + "\r\n";
    ASSERT_TRUE(client.send_cmd(cmd));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("get bigval\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, large_val);
}

TEST_F(MemcacheTest, MultiGet) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    ASSERT_TRUE(client.send_cmd("set mg1 0 0 1\r\na\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");
    ASSERT_TRUE(client.send_cmd("set mg2 0 0 1\r\nb\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");
    ASSERT_TRUE(client.send_cmd("set mg3 0 0 1\r\nc\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    // Multi-get: get mg1 mg2 mg3\r\n
    ASSERT_TRUE(client.send_cmd("get mg1 mg2 mg3\r\n"));

    // Should get VALUE lines for each key, then END.
    std::string all = client.recv_until_end();
    EXPECT_TRUE(all.find("VALUE mg1") != std::string::npos);
    EXPECT_TRUE(all.find("VALUE mg2") != std::string::npos);
    EXPECT_TRUE(all.find("VALUE mg3") != std::string::npos);
}

// Regression: ADD, INCR and APPEND checked or read the key and then wrote
// it in a separate store call, so concurrent clients could both pass the
// check (two ADDs both STORED) or overwrite each other (lost increments).
// The condition is now enforced by the store with the write.
TEST_F(MemcacheTest, ConcurrentAddStoresOnce) {
    constexpr int kClients = 8;
    constexpr int kKeys = 50;
    std::atomic<int> stored[kKeys] = {};
    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&] {
            McClient client;
            if (!client.connect(port_)) return;
            for (int k = 0; k < kKeys; ++k) {
                std::string cmd = "add once" + std::to_string(k) +
                                  " 0 0 1\r\nx\r\n";
                if (!client.send_cmd(cmd)) return;
                if (client.recv_line() == "STORED") stored[k].fetch_add(1);
            }
        });
    }
    for (auto& t : threads) t.join();
    int wrong = 0;
    for (auto& s : stored)
        if (s.load() != 1) ++wrong;
    EXPECT_EQ(wrong, 0) << "keys stored by more or fewer than one ADD";
}

TEST_F(MemcacheTest, ConcurrentIncrLosesNoUpdates) {
    constexpr int kClients = 8;
    constexpr int kIncrs = 100;
    {
        McClient client;
        ASSERT_TRUE(client.connect(port_));
        ASSERT_TRUE(client.send_cmd("set ctr 0 0 1\r\n0\r\n"));
        ASSERT_EQ(client.recv_line(), "STORED");
    }
    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&] {
            McClient client;
            if (!client.connect(port_)) return;
            for (int i = 0; i < kIncrs; ++i) {
                if (!client.send_cmd("incr ctr 1\r\n")) return;
                client.recv_line();
            }
        });
    }
    for (auto& t : threads) t.join();

    McClient client;
    ASSERT_TRUE(client.connect(port_));
    ASSERT_TRUE(client.send_cmd("get ctr\r\n"));
    auto result = client.recv_get_response();
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.data, std::to_string(kClients * kIncrs));
}

TEST_F(MemcacheTest, ConcurrentAppendLosesNoUpdates) {
    constexpr int kClients = 8;
    constexpr int kAppends = 25;
    {
        McClient client;
        ASSERT_TRUE(client.connect(port_));
        ASSERT_TRUE(client.send_cmd("set log 0 0 0\r\n\r\n"));
        ASSERT_EQ(client.recv_line(), "STORED");
    }
    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&] {
            McClient client;
            if (!client.connect(port_)) return;
            for (int i = 0; i < kAppends; ++i) {
                if (!client.send_cmd("append log 0 0 1\r\na\r\n")) return;
                client.recv_line();
            }
        });
    }
    for (auto& t : threads) t.join();

    McClient client;
    ASSERT_TRUE(client.connect(port_));
    ASSERT_TRUE(client.send_cmd("get log\r\n"));
    auto result = client.recv_get_response();
    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.data.size(), size_t{kClients * kAppends});
}


// Regression (PR #3 review, finding 12): reads copied into a fixed buffer
// but decoded the metadata at the end of the full stored size, past the
// buffer. INCR used 140 bytes, so a long value's flags came from the heap.
TEST_F(MemcacheTest, IncrKeepsFlagsOfLongValue) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    std::string val = "5" + std::string(199, ' ');
    ASSERT_TRUE(client.send_cmd("set lc 7 0 " + std::to_string(val.size()) +
                                "\r\n" + val + "\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("incr lc 3\r\n"));
    EXPECT_EQ(client.recv_line(), "8");

    ASSERT_TRUE(client.send_cmd("get lc\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.data, "8");
    EXPECT_EQ(result.flags, 7u);
}

// 4 KiB grains: records up to ~8 MiB, past memcache's 1 MiB limit.
class MemcacheLargeGrainTest : public MemcacheTest {
protected:
    size_t store_size() const override { return 128 * 1024 * 1024 + 4096; }
    uint32_t grain_size() const override { return 4096; }
};

// GET used a 1 MiB buffer: a longer stored value was decoded and sent from
// past its end.
TEST_F(MemcacheLargeGrainTest, GetReturnsWholeValueLongerThanOneMiB) {
    std::string data(1536 * 1024, 'L');
    for (size_t i = 0; i < data.size(); i += 4096) data[i] = 'a' + i % 26;
    // Stored as the server lays it out: data, expiry (never), flags.
    std::string stored = data;
    int64_t expiry = 0;
    uint32_t flags = 9;
    stored.append(reinterpret_cast<const char*>(&expiry), sizeof(expiry));
    stored.append(reinterpret_cast<const char*>(&flags), sizeof(flags));
    ASSERT_EQ(store_.put("huge", stored).run_sync(), 0);

    McClient client;
    ASSERT_TRUE(client.connect(port_));
    ASSERT_TRUE(client.send_cmd("get huge\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.flags, 9u);
    EXPECT_TRUE(result.data == data) << "got " << result.data.size()
                                     << " bytes";
}

// APPEND has the same limit as SET, so values stay readable.
TEST_F(MemcacheLargeGrainTest, AppendBeyondMaxValueIsRefused) {
    McClient client;
    ASSERT_TRUE(client.connect(port_));

    const size_t max = MemcacheServer<uDepot<PosixIO>>::kMaxValueLen;
    std::string val(max, 'M');
    ASSERT_TRUE(client.send_cmd("set full 0 0 " + std::to_string(max) +
                                "\r\n" + val + "\r\n"));
    EXPECT_EQ(client.recv_line(), "STORED");

    ASSERT_TRUE(client.send_cmd("append full 0 0 1\r\nx\r\n"));
    EXPECT_EQ(client.recv_line(), "SERVER_ERROR object too large for cache");

    ASSERT_TRUE(client.send_cmd("get full\r\n"));
    auto result = client.recv_get_response();
    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.data == val);
}

// ─────────────────────────────────────────────────────────────────────────────
// Over the async backends
//
// A connection is a coroutine resumed by its worker's EpollState poller
// thread, so its store operations submit their I/O from that thread, and
// only that thread reaps it (each thread polls its own AIO context or
// io_uring ring, as uDepot's TRT scheduler ran epoll and I/O pollers side by
// side). If the epoll loop did not poll it, every get and set would wait
// forever; the client's receive timeout turns that into a failure.
// ─────────────────────────────────────────────────────────────────────────────
template <typename IO>
class MemcacheAsyncTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("udepot_memcache_async_test_" + std::to_string(getpid()));
        StoreConfig config;
        config.path = path_.c_str();
        config.size = kStoreSize;
        config.grain_size = 512;
        config.force_destroy = true;
        ASSERT_EQ(store_.open(config), 0);
        typename MemcacheServer<uDepot<IO>>::Config mc_config;
        mc_config.bind_addr = "127.0.0.1";
        mc_config.port = 0;
        ASSERT_EQ(server_.start(mc_config), 0);
        port_ = server_.port();
        ASSERT_GT(port_, 0);
    }

    void TearDown() override {
        Watchdog watchdog("stop()", std::chrono::seconds(30));
        server_.stop();
        store_.close();
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
    uDepot<IO> store_;
    MemcacheServer<uDepot<IO>> server_{store_};
    uint16_t port_ = 0;
};

#ifdef UDEPOT_BUILD_URING
using AsyncBackends = ::testing::Types<udepot::AioIO, udepot::UringIO>;
#else
using AsyncBackends = ::testing::Types<udepot::AioIO>;
#endif
TYPED_TEST_SUITE(MemcacheAsyncTest, AsyncBackends);

// Several connections at once, served by the server's workers, each
// setting, getting, overwriting and deleting keys of its own.
TYPED_TEST(MemcacheAsyncTest, ConnectionsDoTheirIoOnTheirPollers) {
    constexpr int kConns = 3;
    constexpr int kKeys = 50;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int c = 0; c < kConns; ++c) {
        threads.emplace_back([&, c] {
            McClient client;
            if (!client.connect(this->port_)) { ++failures; return; }
            auto key = [&](int i) {
                return "c" + std::to_string(c) + "k" + std::to_string(i);
            };
            auto val = [&](int i, char fill) {
                return std::string(100 + i * 7, fill);
            };
            for (char fill : {'a', 'b'}) {
                for (int i = 0; i < kKeys; ++i) {
                    const std::string v = val(i, fill);
                    client.send_cmd("set " + key(i) + " 0 0 " +
                                    std::to_string(v.size()) + "\r\n" + v +
                                    "\r\n");
                    if (client.recv_line() != "STORED") { ++failures; return; }
                }
                for (int i = 0; i < kKeys; ++i) {
                    client.send_cmd("get " + key(i) + "\r\n");
                    auto r = client.recv_get_response();
                    if (!r.found || r.data != val(i, fill)) {
                        ++failures;
                        return;
                    }
                }
            }
            for (int i = 0; i < kKeys; ++i) {
                client.send_cmd("delete " + key(i) + "\r\n");
                if (client.recv_line() != "DELETED") { ++failures; return; }
                client.send_cmd("get " + key(i) + "\r\n");
                if (client.recv_line() != "END") { ++failures; return; }
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(failures.load(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// The worker pool: connections are coroutines on a fixed set of worker
// threads, so the threads, and the AIO contexts / io_uring rings that go
// with them, do not grow with the connections.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

bool wait_until(const std::function<bool()>& pred,
                std::chrono::seconds timeout = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

size_t open_fds() {
    size_t n = 0;
    for ([[maybe_unused]] const auto& e :
         std::filesystem::directory_iterator("/proc/self/fd"))
        ++n;
    return n;
}

template <typename IO>
unsigned live_io_contexts() {
    if constexpr (std::is_same_v<IO, udepot::AioIO>)
        return udepot::AioIO::live_contexts();
#ifdef UDEPOT_BUILD_URING
    else
        return udepot::UringIO::live_rings();
#endif
}

// Whether the fds above `fds_before` are exactly the io_uring rings (each
// an fd) that exist above `base`: the store's own threads' rings, which
// outlive the server. AIO contexts are not fds.
template <typename IO>
bool fds_match_contexts(size_t fds_before, unsigned base) {
    const unsigned extra = live_io_contexts<IO>() - base;
    const size_t extra_fds = std::is_same_v<IO, udepot::AioIO> ? 0 : extra;
    return open_fds() == fds_before + extra_fds;
}

// `n` sets then `n` gets of keys of its own; whether every one was right.
bool sets_and_gets(McClient& client, int id, int n) {
    for (int i = 0; i < n; ++i) {
        const std::string v(64 + i, static_cast<char>('a' + id % 26));
        if (!client.send_cmd("set p" + std::to_string(id) + "_" +
                             std::to_string(i) + " 0 0 " +
                             std::to_string(v.size()) + "\r\n" + v + "\r\n"))
            return false;
        if (client.recv_line() != "STORED") return false;
    }
    for (int i = 0; i < n; ++i) {
        const std::string v(64 + i, static_cast<char>('a' + id % 26));
        if (!client.send_cmd("get p" + std::to_string(id) + "_" +
                             std::to_string(i) + "\r\n"))
            return false;
        auto r = client.recv_get_response();
        if (!r.found || r.data != v) return false;
    }
    return true;
}

}  // namespace

// The worker count is the config's, 0 meaning one per hardware thread (and
// at least one), and stopping and starting again builds the pool afresh.
TEST_F(MemcacheTest, WorkerCountIsHonoured) {
    EXPECT_EQ(server_.workers(),
              std::max(1u, std::thread::hardware_concurrency()))
        << "default (workers = 0)";

    server_.stop();
    EXPECT_EQ(server_.workers(), 0u) << "stopped";
    MemcacheServer<uDepot<PosixIO>>::Config mc_config;
    mc_config.port = 0;
    mc_config.workers = 3;
    ASSERT_EQ(server_.start(mc_config), 0);
    EXPECT_EQ(server_.workers(), 3u);

    McClient client;
    ASSERT_TRUE(client.connect(server_.port()));
    EXPECT_TRUE(sets_and_gets(client, 1, 3));

    server_.stop();
    mc_config.workers = 1;
    ASSERT_EQ(server_.start(mc_config), 0);
    EXPECT_EQ(server_.workers(), 1u);
    McClient again;
    ASSERT_TRUE(again.connect(server_.port()));
    EXPECT_TRUE(sets_and_gets(again, 2, 3));
}

// A finished connection is reclaimed (its coroutine frame, its slot) and its
// fd closed exactly once, however it finished; the pool keeps serving.
TEST_F(MemcacheTest, FinishedConnectionsAreReclaimed) {
    const size_t fds_before = open_fds();
    constexpr int kConns = 50;
    {
        std::vector<McClient> clients(kConns);
        for (int i = 0; i < kConns; ++i) {
            ASSERT_TRUE(clients[i].connect(port_));
            ASSERT_TRUE(sets_and_gets(clients[i], i, 2));
        }
        EXPECT_TRUE(wait_until(
            [&] { return server_.connections() == size_t{kConns}; }))
            << "connections() = " << server_.connections();
    }  // every client closes
    EXPECT_TRUE(wait_until([&] { return server_.connections() == 0; }))
        << "finished connections not reclaimed: " << server_.connections();
    // The server's end of each is closed too (the clients' are gone).
    EXPECT_TRUE(wait_until([&] { return open_fds() == fds_before; }))
        << "fds: " << open_fds() << " before: " << fds_before;
}

// Connections that end before (or as) their worker starts them: connect and
// close at once, many times. The coroutine can finish inside its own eager
// start, before its task is stored.
TEST_F(MemcacheTest, ConnectionsClosedAtOnceAreReclaimed) {
    const size_t fds_before = open_fds();
    for (int i = 0; i < 300; ++i) {
        McClient c;
        ASSERT_TRUE(c.connect(port_));
    }
    EXPECT_TRUE(wait_until([&] { return server_.connections() == 0; }))
        << "connections() = " << server_.connections();
    EXPECT_TRUE(wait_until([&] { return open_fds() == fds_before; }))
        << "fds: " << open_fds() << " before: " << fds_before;
    McClient client;
    ASSERT_TRUE(client.connect(port_));
    EXPECT_TRUE(sets_and_gets(client, 7, 3));
}

template <typename IO>
class MemcachePoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("udepot_memcache_pool_test_" + std::to_string(getpid()));
        StoreConfig config;
        config.path = path_.c_str();
        // Large enough that the store's own GC thread, which does I/O of its
        // own, has nothing to do: the tests count contexts.
        config.size = store_size();
        config.grain_size = 512;
        config.force_destroy = true;
        ASSERT_EQ(store_.open(config), 0);
        // This thread's context exists before the tests count them.
        ASSERT_EQ(store_.put("warm", "x").run_sync(), 0);
    }

    void TearDown() override {
        {
            Watchdog watchdog("stop()", std::chrono::seconds(30));
            server_.stop();
        }
        store_.close();
        std::filesystem::remove(path_);
    }

    virtual size_t store_size() const { return size_t{64} * 1024 * 1024; }

    void start(unsigned workers) {
        typename MemcacheServer<uDepot<IO>>::Config mc_config;
        mc_config.port = 0;
        mc_config.workers = workers;
        // Hundreds of clients connect at once; the default backlog (128)
        // would drop SYNs and stall them for a second or more.
        mc_config.backlog = 1024;
        ASSERT_EQ(server_.start(mc_config), 0);
        port_ = server_.port();
    }

    std::filesystem::path path_;
    uDepot<IO> store_;
    MemcacheServer<uDepot<IO>> server_{store_};
    uint16_t port_ = 0;
};
TYPED_TEST_SUITE(MemcachePoolTest, AsyncBackends);

// 200 connections held open at once, each with I/O of its own, on 4 workers:
// all succeed, and the server holds exactly 4 AIO contexts / rings (each
// worker did I/O, so each took one; round robin spreads the connections),
// not one or two per connection, which at the default queue depth would
// exhaust fs.aio-max-nr at about 32 connections. They go when the pool does.
TYPED_TEST(MemcachePoolTest, ManyConnectionsShareTheWorkersContexts) {
    constexpr unsigned kWorkers = 4;
    constexpr int kConns = 200;
    const unsigned base = live_io_contexts<TypeParam>();
    this->start(kWorkers);
    ASSERT_EQ(this->server_.workers(), kWorkers);

    std::atomic<int> connected{0};
    std::atomic<int> failures{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int c = 0; c < kConns; ++c) {
        threads.emplace_back([&, c] {
            McClient client;
            if (!client.connect(this->port_)) { ++failures; ++connected; return; }
            ++connected;
            while (!go) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!sets_and_gets(client, c, 5)) ++failures;
        });
    }
    const bool all_held = wait_until([&] {
        return connected == kConns &&
               this->server_.connections() == size_t{kConns};
    });
    go = true;
    for (auto& t : threads) t.join();
    EXPECT_TRUE(all_held) << "connections() = " << this->server_.connections();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(live_io_contexts<TypeParam>() - base, kWorkers);

    this->server_.stop();
    EXPECT_EQ(live_io_contexts<TypeParam>(), base)
        << "the workers' contexts outlived the server";
}

// stop() with connections open, some idle and some with store I/O in flight:
// it returns (the watchdog ends the process, failing the test, if not), every
// worker and connection is gone, no fd is left, and the contexts are freed.
TYPED_TEST(MemcachePoolTest, StopWithConnectionsOpenAndIoInFlight) {
    constexpr int kIdle = 20;
    constexpr int kBusy = 20;
    const unsigned base = live_io_contexts<TypeParam>();
    const size_t fds_before = open_fds();
    this->start(2);

    std::vector<McClient> idle(kIdle);
    for (auto& c : idle) ASSERT_TRUE(c.connect(this->port_));
    std::atomic<int> busy_started{0};
    std::atomic<bool> stop_clients{false};
    std::vector<std::thread> busy;
    const std::string v(32 * 1024, 'z');
    for (int b = 0; b < kBusy; ++b) {
        busy.emplace_back([&, b] {
            McClient client;
            if (!client.connect(this->port_)) return;
            const std::string key = "b" + std::to_string(b);
            if (!client.send_cmd("set " + key + " 0 0 " +
                                 std::to_string(v.size()) + "\r\n" + v + "\r\n") ||
                client.recv_line() != "STORED")
                return;
            ++busy_started;
            // Reads back to back, each a store read in flight, until the
            // server goes away. (Reads, not writes: a writer fills the
            // store, and the store's own GC thread would then take a
            // context of its own, which the test counts.)
            while (!stop_clients) {
                if (!client.send_cmd("get " + key + "\r\n")) return;
                auto r = client.recv_get_response();
                if (!r.found) return;
            }
        });
    }
    ASSERT_TRUE(wait_until([&] { return busy_started == kBusy; }));
    ASSERT_TRUE(wait_until([&] {
        return this->server_.connections() == size_t{kIdle + kBusy};
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
        Watchdog watchdog("stop() with connections open", std::chrono::seconds(30));
        this->server_.stop();
    }
    stop_clients = true;
    for (auto& t : busy) t.join();
    idle.clear();

    EXPECT_FALSE(this->server_.is_running());
    EXPECT_EQ(this->server_.workers(), 0u);
    EXPECT_EQ(this->server_.connections(), 0u);
    EXPECT_EQ(live_io_contexts<TypeParam>(), base);
    EXPECT_EQ(open_fds(), fds_before) << "fds leaked by stop()";
}

// ─────────────────────────────────────────────────────────────────────────────
// Hand-off, start-up failure and mid-flight tests
// ─────────────────────────────────────────────────────────────────────────────

namespace {

using udepot::EpollState;
using udepot::MemcacheTestHooks;

// The server's own threads: those whose name the server set (the accept
// thread "udepot-mc-acc", each worker "udepot-mc-w<N>"). Counting the
// process's threads instead measured threads that are not the server's too
// (the store's RCU reclaimer starts lazily) and ones the kernel still counts
// after pthread_join returned, until it reaps them.
size_t server_threads() {
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/task")) {
        std::ifstream f(e.path() / "comm");
        std::string comm;
        std::getline(f, comm);
        if (comm.rfind("udepot-mc-", 0) == 0) ++n;
    }
    return n;
}

// Whether the server's thread count reaches `n` (within 2 s, which absorbs
// the kernel's lag in reaping a joined thread; a leaked thread never goes
// away, so it still fails). Zero is checked again after a moment: a thread
// that was just started and is not yet named would otherwise hide.
bool server_threads_reach(size_t n) {
    if (!wait_until([&] { return server_threads() == n; },
                    std::chrono::seconds(2)))
        return false;
    if (n != 0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return server_threads() == 0;
}

// Clears every seam, whatever the test did.
struct ResetHooks {
    ~ResetHooks() {
        MemcacheTestHooks::socketpair_test_hook = nullptr;
        MemcacheTestHooks::handoff_recv_test_hook = nullptr;
        MemcacheTestHooks::connection_finished_test_hook = nullptr;
        EpollState::init_test_hook = nullptr;
    }
};

std::atomic<bool> g_stall_release{false};
int stall_worker(unsigned) {
    while (!g_stall_release)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return 0;
}

std::atomic<int> g_hook_calls{0};
int fail_first_recv(unsigned) { return g_hook_calls.fetch_add(1) == 0 ? EIO : 0; }
int fail_socketpair_at_1(unsigned worker) { return worker == 1 ? EMFILE : 0; }
int fail_third_epoll_init() { return g_hook_calls.fetch_add(1) == 2 ? EMFILE : 0; }

}  // namespace

// A worker that stops draining its hand-off socket (here: stalled) must not
// block the accept thread: the connections it cannot take are closed, said
// so, and counted, and stop() still returns. With a blocking send the accept
// thread sat in it, never counting a refusal, and stop() joined it forever.
TEST_F(MemcacheTest, StalledWorkerDoesNotBlockAcceptOrStop) {
    server_.stop();
    ResetHooks reset;
    g_stall_release = false;
    MemcacheTestHooks::handoff_recv_test_hook = stall_worker;
    const size_t fds_before = open_fds();

    MemcacheServer<uDepot<PosixIO>>::Config mc_config;
    mc_config.port = 0;
    mc_config.workers = 1;
    mc_config.backlog = 1024;
    ASSERT_EQ(server_.start(mc_config), 0);

    ::testing::internal::CaptureStderr();
    // The worker takes the first connection and stalls; the rest queue in
    // its socket until that is full.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (server_.refused_connections() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        McClient c;
        c.connect(server_.port());
    }
    const uint64_t refused = server_.refused_connections();
    g_stall_release = true;
    {
        Watchdog watchdog("stop() with a stalled worker", std::chrono::seconds(30));
        server_.stop();
    }
    const std::string log = ::testing::internal::GetCapturedStderr();
    EXPECT_GT(refused, 0u) << "a full hand-off queue never refused a connection";
    EXPECT_NE(log.find("udepot: memcache: no worker could take a connection"),
              std::string::npos) << log;
    EXPECT_TRUE(wait_until([&] { return open_fds() == fds_before; }))
        << "fds: " << open_fds() << " before: " << fds_before;
}

// A hand-off receive that fails: said, the connection received and those
// still queued are closed, and the worker takes no more. The accept thread
// then finds its socket broken, says so, and (as no worker is left) refuses.
TEST_F(MemcacheTest, HandoffRecvFailureIsReportedAndCleanedUp) {
    server_.stop();
    ResetHooks reset;
    g_hook_calls = 0;
    MemcacheTestHooks::handoff_recv_test_hook = fail_first_recv;
    const size_t fds_before = open_fds();

    MemcacheServer<uDepot<PosixIO>>::Config mc_config;
    mc_config.port = 0;
    mc_config.workers = 1;
    ASSERT_EQ(server_.start(mc_config), 0);

    ::testing::internal::CaptureStderr();
    {
        McClient first;
        ASSERT_TRUE(first.connect(server_.port()));
        // The server closes it: the client reads EOF.
        EXPECT_EQ(first.recv_line(), "");
    }
    // The worker is gone; connect until the accept thread has seen it.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (server_.refused_connections() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        McClient c;
        c.connect(server_.port());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    {
        Watchdog watchdog("stop() after a worker failed", std::chrono::seconds(30));
        server_.stop();
    }
    const std::string log = ::testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("udepot: memcache worker 0: hand-off socket failed (Input/output error)"),
              std::string::npos) << log;
    EXPECT_NE(log.find("udepot: memcache: could not hand a connection to worker 0 (Broken pipe)"),
              std::string::npos) << log;
    EXPECT_NE(log.find("udepot: memcache: no worker could take a connection"),
              std::string::npos) << log;
    EXPECT_GT(server_.refused_connections(), 0u);
    EXPECT_TRUE(wait_until([&] { return open_fds() == fds_before; }))
        << "fds: " << open_fds() << " before: " << fds_before;

    // Nothing is stuck: a fresh start works.
    MemcacheTestHooks::handoff_recv_test_hook = nullptr;
    ASSERT_EQ(server_.start(mc_config), 0);
    McClient client;
    ASSERT_TRUE(client.connect(server_.port()));
    EXPECT_TRUE(sets_and_gets(client, 4, 2));
}

// A start() that fails part-way (the third worker's epoll, the second's
// socketpair) says which and why, returns the error, leaves no thread, fd
// or port behind, and a later start() works.
TEST_F(MemcacheTest, FailedStartRollsBackAndSaysSo) {
    server_.stop();
    ResetHooks reset;
    MemcacheServer<uDepot<PosixIO>>::Config mc_config;
    mc_config.port = 0;
    mc_config.workers = 3;
    const size_t fds_before = open_fds();
    ASSERT_TRUE(server_threads_reach(0)) << "server threads after stop()";

    struct Case {
        const char* what;
        std::function<void()> arm;
        int expect_rc;
        const char* message;
    };
    const Case cases[] = {
        {"socketpair of worker 1",
         [] { MemcacheTestHooks::socketpair_test_hook = fail_socketpair_at_1; },
         -EMFILE,
         "udepot: memcache worker 1: socketpair: Too many open files"},
        {"epoll of worker 2",
         [] {
             g_hook_calls = 0;
             EpollState::init_test_hook = fail_third_epoll_init;
         },
         -EMFILE,
         "udepot: memcache worker 2: epoll: Too many open files"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.what);
        c.arm();
        ::testing::internal::CaptureStderr();
        const int rc = server_.start(mc_config);
        const std::string log = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(rc, c.expect_rc);
        EXPECT_NE(log.find(c.message), std::string::npos) << log;
        EXPECT_FALSE(server_.is_running());
        EXPECT_EQ(server_.workers(), 0u);
        EXPECT_EQ(server_.port(), 0u) << "port_ not reset";
        EXPECT_EQ(open_fds(), fds_before);
        EXPECT_TRUE(server_threads_reach(0)) << "threads left by a failed start";
        MemcacheTestHooks::socketpair_test_hook = nullptr;
        EpollState::init_test_hook = nullptr;

        // And the server can be started afterwards.
        ASSERT_EQ(server_.start(mc_config), 0);
        EXPECT_EQ(server_.workers(), 3u);
        // The accept thread and the 3 workers, each named by the server.
        EXPECT_TRUE(server_threads_reach(4))
            << "server threads: " << server_threads();
        McClient client;
        ASSERT_TRUE(client.connect(server_.port()));
        EXPECT_TRUE(sets_and_gets(client, 5, 2));
        client.close();
        server_.stop();
        EXPECT_EQ(open_fds(), fds_before);
        EXPECT_TRUE(server_threads_reach(0)) << "threads left by stop()";
    }
}

namespace {
thread_local bool tl_is_worker = false;
std::atomic<uint64_t> g_off_worker_finishes{0};
std::atomic<uint64_t> g_finishes{0};
int mark_worker(unsigned) {
    tl_is_worker = true;
    return 0;
}
void count_finish() {
    ++g_finishes;
    if (!tl_is_worker) ++g_off_worker_finishes;
}
}  // namespace

// A small store, so puts run out of space and wait (SpaceWait); the store's
// waker thread then resumes the connection's coroutine, which runs on until
// its next socket wait. The client has already closed, so the coroutine's
// recv sees EOF at once and the connection FINISHES ON THE WAKER THREAD, not
// on its worker. It must still be reclaimed by its worker, its fd closed once,
// nothing left. The test knows the path was taken: the finish seam runs on
// the finishing thread, and worker threads are the ones the hand-off seam ran
// on (it marks them), so a finish on an unmarked thread is the waker's.
template <typename IO>
class MemcacheWakerTest : public MemcachePoolTest<IO> {
protected:
    size_t store_size() const override { return kStoreSize; }
};
TYPED_TEST_SUITE(MemcacheWakerTest, AsyncBackends);

TYPED_TEST(MemcacheWakerTest, ConnectionFinishingOnTheWakerThreadIsReclaimed) {
    ResetHooks reset;
    g_off_worker_finishes = 0;
    g_finishes = 0;
    MemcacheTestHooks::handoff_recv_test_hook = mark_worker;
    MemcacheTestHooks::connection_finished_test_hook = count_finish;
    const unsigned base = live_io_contexts<TypeParam>();
    const size_t fds_before = open_fds();
    this->start(2);

    std::atomic<bool> stop_clients{false};
    std::vector<std::thread> clients;
    const std::string v(32 * 1024, 'w');
    for (int t = 0; t < 4; ++t) {
        clients.emplace_back([&, t] {
            for (int i = 0; !stop_clients; ++i) {
                McClient c;
                if (!c.connect(this->port_)) continue;
                // Send the whole set and close without reading the reply.
                c.send_cmd("set w" + std::to_string((t + i) % 8) + " 0 0 " +
                           std::to_string(v.size()) + "\r\n" + v + "\r\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    const bool taken =
        wait_until([&] { return g_off_worker_finishes.load() > 0; },
                   std::chrono::seconds(60));
    stop_clients = true;
    for (auto& t : clients) t.join();
    EXPECT_TRUE(taken) << "no connection ever finished on the waker thread ("
                       << g_finishes.load() << " finished on workers)";

    EXPECT_TRUE(wait_until([&] { return this->server_.connections() == 0; }))
        << "connections() = " << this->server_.connections();
    {
        Watchdog watchdog("stop()", std::chrono::seconds(30));
        this->server_.stop();
    }
    // The workers' contexts are gone; what is left is the store's own
    // threads' (its waker, GC), which live as long as the store. An io_uring
    // ring is an fd, so those account for the extra fds exactly.
    EXPECT_LE(live_io_contexts<TypeParam>() - base, 2u);
    // The store's own threads (GC) may still be starting a ring of their own
    // (an fd) between two readings, so read both together until they agree;
    // a leaked fd never does.
    EXPECT_TRUE(wait_until([&] { return fds_match_contexts<TypeParam>(fds_before, base); },
                           std::chrono::seconds(5)))
        << "fds: " << open_fds() << " before: " << fds_before
        << " contexts above base: " << live_io_contexts<TypeParam>() - base;
}

namespace {
std::atomic<bool> g_get_entered{false};
std::atomic<bool> g_get_release{false};
void hold_get(std::span<const uint8_t>) {
    g_get_entered = true;
    while (!g_get_release)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
}  // namespace

// stop() while an operation is executing: a get is held (by the store's
// get_read_test_hook) after its lookup and before its read is submitted, on
// the worker's thread; stop() is called then and released only afterwards,
// so the read is submitted and completes with stop() already under way.
// stop() must wait for it (it cannot return while the worker is inside the
// get), then finish: no hang, nothing left. (The earlier stop test only
// makes it likely that I/O is in flight; no hook delays an I/O's completion.)
TYPED_TEST(MemcachePoolTest, StopWaitsForAnOperationInFlight) {
    using Store = uDepot<TypeParam>;
    struct Reset {
        ~Reset() {
            g_get_release = true;
            Store::get_read_test_hook = nullptr;
        }
    } reset;
    g_get_entered = false;
    g_get_release = false;
    const unsigned base = live_io_contexts<TypeParam>();
    const size_t fds_before = open_fds();
    this->start(1);

    McClient client;
    ASSERT_TRUE(client.connect(this->port_));
    ASSERT_TRUE(client.send_cmd("set held 0 0 5\r\nhello\r\n"));
    ASSERT_EQ(client.recv_line(), "STORED");
    Store::get_read_test_hook = hold_get;
    ASSERT_TRUE(client.send_cmd("get held\r\n"));
    ASSERT_TRUE(wait_until([&] { return g_get_entered.load(); }));

    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        this->server_.stop();
        stopped = true;
    });
    Watchdog watchdog("stop() with a get in progress", std::chrono::seconds(30));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(stopped) << "stop() returned with an operation executing";
    g_get_release = true;
    EXPECT_TRUE(wait_until([&] { return stopped.load(); }));
    stopper.join();

    EXPECT_EQ(this->server_.connections(), 0u);
    EXPECT_EQ(this->server_.workers(), 0u);
    client.close();
    EXPECT_EQ(open_fds(), fds_before);
    EXPECT_EQ(live_io_contexts<TypeParam>(), base);
}

namespace {
std::atomic<int> g_waker_held{0};
std::atomic<bool> g_waker_release{false};
// The store's space waker waits here, holding the operations it is about to
// wake parked in SpaceWait, until the test releases it.
void hold_waker() {
    ++g_waker_held;
    while (!g_waker_release)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
}  // namespace

// stop() while a connection is parked in the store's SpaceWait: a client
// overwrites keys on a small store until a put waits for space, and the
// store's waker is held (space_waker_test_hook) with that put taken but not
// resumed. stop() is then called. It must wait for the parked operation and
// return only after the waker is released and the connection has finished
// and been reclaimed. Without join_poller(), es.stop() forces the worker's
// poller out while the connection is alive and the Worker is destroyed under
// it.
TYPED_TEST(MemcacheWakerTest, StopWaitsForAConnectionParkedInSpaceWait) {
    using Store = uDepot<TypeParam>;
    struct Reset {
        ~Reset() {
            g_waker_release = true;
            Store::space_waker_test_hook = nullptr;
        }
    } reset;
    g_waker_held = 0;
    g_waker_release = false;
    const unsigned base = live_io_contexts<TypeParam>();
    const size_t fds_before = open_fds();
    this->start(1);
    Store::space_waker_test_hook = hold_waker;

    // Mostly live data, so little is free and GC has to work for every
    // segment, then several connections overwriting keys at once: puts run
    // out of space ahead of GC and wait for it.
    {
        McClient c;
        ASSERT_TRUE(c.connect(this->port_));
        const std::string v(32 * 1024, 'L');
        for (int i = 0; i < 64; ++i) {
            ASSERT_TRUE(c.send_cmd("set L" + std::to_string(i) + " 0 0 " +
                                   std::to_string(v.size()) + "\r\n" + v + "\r\n"));
            ASSERT_EQ(c.recv_line(), "STORED") << "live fill " << i;
        }
    }
    std::atomic<bool> stop_filler{false};
    std::vector<std::thread> fillers;
    for (int t = 0; t < 6; ++t) {
        fillers.emplace_back([&, t] {
            McClient c;
            if (!c.connect(this->port_)) return;
            const std::string v(32 * 1024, 'f');
            for (int i = 0; !stop_filler; ++i) {
                if (!c.send_cmd("set f" + std::to_string((t + i) % 8) + " 0 0 " +
                                std::to_string(v.size()) + "\r\n" + v + "\r\n"))
                    return;
                if (c.recv_line() != "STORED") return;
            }
        });
    }
    EXPECT_TRUE(wait_until([&] { return g_waker_held.load() > 0; }))
        << "no put ever waited for space";
    EXPECT_GE(this->server_.connections(), 1u);

    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        this->server_.stop();
        stopped = true;
    });
    {
        Watchdog watchdog("stop() with a connection in SpaceWait",
                          std::chrono::seconds(60));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        EXPECT_FALSE(stopped) << "stop() returned with an operation parked";
        g_waker_release = true;  // the waker resumes the put
        EXPECT_TRUE(wait_until([&] { return stopped.load(); }))
            << "stop() did not finish once the waker was released";
        stopper.join();
        stop_filler = true;
        for (auto& f : fillers) f.join();
    }
    EXPECT_EQ(this->server_.connections(), 0u);
    EXPECT_EQ(this->server_.workers(), 0u);
    EXPECT_LE(live_io_contexts<TypeParam>() - base, 2u);  // the store's own threads
    EXPECT_TRUE(wait_until([&] { return fds_match_contexts<TypeParam>(fds_before, base); },
                           std::chrono::seconds(5)))
        << "fds: " << open_fds() << " before: " << fds_before
        << " contexts above base: " << live_io_contexts<TypeParam>() - base;
}
