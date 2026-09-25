// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "net/memcache.h"
#include "udepot/store.h"
#include "udepot/io/posix.h"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
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
using udepot::UDepot;

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

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture
// ─────────────────────────────────────────────────────────────────────────────
class MemcacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                "udepot_memcache_test";
        StoreConfig config;
        config.path = path_.c_str();
        config.size = kStoreSize;
        config.grain_size = 512;
        config.initial_tables = 2;
        config.index_bits = 10;
        config.force_destroy = true;
        ASSERT_EQ(store_.open(config), 0);

        MemcacheServer<UDepot<PosixIO>>::Config mc_config;
        mc_config.bind_addr = "127.0.0.1";
        mc_config.port = 0;  // Ephemeral port.
        ASSERT_EQ(server_.start(mc_config), 0);
        port_ = server_.port();
        ASSERT_GT(port_, 0);

        // Give the accept thread time to be ready.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void TearDown() override {
        server_.stop();
        store_.close();
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
    UDepot<PosixIO> store_;
    MemcacheServer<UDepot<PosixIO>> server_{store_};
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
