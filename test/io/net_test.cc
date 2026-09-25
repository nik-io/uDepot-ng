// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/net.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

using udepot::Connection;
using udepot::EpollState;

class EpollNetTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(es_.init(), 0);
    }

    void TearDown() override {
        es_.stop();
    }

    // Helper: create a TCP listening socket on loopback, bind to an
    // ephemeral port, and return the fd and the assigned port.
    static std::pair<int, uint16_t> make_listener() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(fd, 0);

        int optval = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

        socklen_t len = sizeof(addr);
        getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        return {fd, ntohs(addr.sin_port)};
    }

    // Helper: connect a blocking client socket to loopback:port.
    static int connect_to(uint16_t port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc < 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    EpollState es_;
};

TEST_F(EpollNetTest, InitAndStop) {
    EXPECT_TRUE(es_.is_running());
}

TEST_F(EpollNetTest, AcceptAndEcho) {
    auto [listen_fd, port] = make_listener();
    ASSERT_EQ(es_.listen(listen_fd, 5), 0);

    // Spawn a client thread that connects, sends data, and reads the echo.
    std::string received;
    std::thread client([&] {
        int fd = connect_to(port);
        ASSERT_GE(fd, 0);
        const char* msg = "hello epoll";
        ::send(fd, msg, strlen(msg), 0);
        char buf[64] = {};
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) received.assign(buf, static_cast<size_t>(n));
        ::close(fd);
    });

    // Server side: accept, recv, send echo, close.
    struct sockaddr_in cli_addr{};
    socklen_t cli_len = sizeof(cli_addr);
    auto accept_aw = es_.accept_ll(listen_fd,
                                   reinterpret_cast<sockaddr*>(&cli_addr),
                                   &cli_len);
    // Drive the accept awaitable synchronously (it's an eager coroutine).
    // We wrap it in a CoroTask to use run_sync().
    auto accept_task = [&]() -> udepot::CoroTask<int> {
        ssize_t afd = co_await std::move(accept_aw);
        if (afd < 0) co_return -1;

        int fd = static_cast<int>(afd);
        es_.register_fd(fd, EPOLLIN);
        int optval = 1;
        setsockopt(fd, SOL_TCP, TCP_NODELAY, &optval, sizeof(optval));

        Connection conn(es_, fd);
        char buf[64] = {};
        ssize_t n = co_await conn.recv(buf, sizeof(buf), 0);
        if (n > 0) {
            co_await conn.send(buf, static_cast<size_t>(n), 0);
        }
        es_.close_fd(fd);
        co_return 0;
    }();
    int rc = accept_task.run_sync();
    EXPECT_EQ(rc, 0);

    client.join();
    EXPECT_EQ(received, "hello epoll");
    es_.close_fd(listen_fd);
}

TEST_F(EpollNetTest, RecvFullSendFull) {
    auto [listen_fd, port] = make_listener();
    ASSERT_EQ(es_.listen(listen_fd, 5), 0);

    const std::string message(4096, 'X');

    std::string received;
    std::thread client([&] {
        int fd = connect_to(port);
        ASSERT_GE(fd, 0);
        // Send in small chunks to exercise recv_full's retry loop.
        size_t sent = 0;
        while (sent < message.size()) {
            size_t chunk = std::min<size_t>(100, message.size() - sent);
            ssize_t n = ::send(fd, message.data() + sent, chunk, 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        // Read the echo back.
        received.resize(message.size());
        size_t got = 0;
        while (got < message.size()) {
            ssize_t n = ::recv(fd, received.data() + got,
                               message.size() - got, 0);
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        received.resize(got);
        ::close(fd);
    });

    auto server_task = [&]() -> udepot::CoroTask<int> {
        struct sockaddr_in cli_addr{};
        socklen_t cli_len = sizeof(cli_addr);
        ssize_t afd = co_await es_.accept_ll(
            listen_fd, reinterpret_cast<sockaddr*>(&cli_addr), &cli_len);
        if (afd < 0) co_return -1;

        int fd = static_cast<int>(afd);
        es_.register_fd(fd, EPOLLIN | EPOLLOUT);

        Connection conn(es_, fd);
        std::vector<char> buf(message.size());
        int rc = co_await conn.recv_full(buf.data(), buf.size(), 0);
        if (rc != 0) { es_.close_fd(fd); co_return rc; }

        rc = co_await conn.send_full(buf.data(), buf.size(), 0);
        es_.close_fd(fd);
        co_return rc;
    }();

    int rc = server_task.run_sync();
    EXPECT_EQ(rc, 0);

    client.join();
    EXPECT_EQ(received, message);
    es_.close_fd(listen_fd);
}

TEST_F(EpollNetTest, MultipleClients) {
    auto [listen_fd, port] = make_listener();
    ASSERT_EQ(es_.listen(listen_fd, 10), 0);

    constexpr int kNumClients = 5;
    std::vector<std::string> results(kNumClients);

    // Spawn client threads.
    std::vector<std::thread> clients;
    for (int i = 0; i < kNumClients; ++i) {
        clients.emplace_back([&, i] {
            int fd = connect_to(port);
            if (fd < 0) return;
            std::string msg = "client_" + std::to_string(i);
            ::send(fd, msg.data(), msg.size(), 0);
            char buf[64] = {};
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) results[i].assign(buf, static_cast<size_t>(n));
            ::close(fd);
        });
    }

    // Accept and echo each client serially.
    for (int i = 0; i < kNumClients; ++i) {
        auto task = [&]() -> udepot::CoroTask<int> {
            struct sockaddr_in cli_addr{};
            socklen_t cli_len = sizeof(cli_addr);
            ssize_t afd = co_await es_.accept_ll(
                listen_fd, reinterpret_cast<sockaddr*>(&cli_addr), &cli_len);
            if (afd < 0) co_return -1;

            int fd = static_cast<int>(afd);
            es_.register_fd(fd, EPOLLIN);

            Connection conn(es_, fd);
            char buf[64] = {};
            ssize_t n = co_await conn.recv(buf, sizeof(buf), 0);
            if (n > 0)
                co_await conn.send(buf, static_cast<size_t>(n), 0);
            es_.close_fd(fd);
            co_return 0;
        }();
        EXPECT_EQ(task.run_sync(), 0);
    }

    for (auto& t : clients) t.join();

    // Verify each client got its own message echoed.
    for (int i = 0; i < kNumClients; ++i) {
        EXPECT_FALSE(results[i].empty()) << "client " << i << " got no data";
    }
    es_.close_fd(listen_fd);
}
