#pragma once

#include <atomic>
#include <cstddef>
#include <sys/types.h>
#include <thread>

#include <linux/aio_abi.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"

namespace udepot {

class AioIO {
public:
    AioIO() noexcept = default;
    ~AioIO();

    AioIO(const AioIO&) = delete;
    AioIO& operator=(const AioIO&) = delete;

    int open(const char* path, size_t size);
    void close();

    CoroTask<ssize_t> pread(void* buf, size_t count, off_t offset);
    CoroTask<ssize_t> pwrite(const void* buf, size_t count, off_t offset);

    size_t get_size() const noexcept { return size_; }
    IoBuffer alloc_buffer(size_t size);

private:
    int fd_ = -1;
    size_t size_ = 0;
    aio_context_t ctx_ = 0;
    std::thread poller_;
    std::atomic<bool> running_{false};

    void poller_loop();
};

}  // namespace udepot
