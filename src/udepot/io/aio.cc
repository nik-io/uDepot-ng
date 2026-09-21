#include "udepot/io/aio.h"

#include <cerrno>
#include <coroutine>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace udepot {

static inline int sys_io_setup(unsigned nr, aio_context_t* ctx) {
    return static_cast<int>(syscall(SYS_io_setup, nr, ctx));
}

static inline int sys_io_destroy(aio_context_t ctx) {
    return static_cast<int>(syscall(SYS_io_destroy, ctx));
}

static inline int sys_io_submit(aio_context_t ctx, long nr,
                                struct iocb** iocbs) {
    return static_cast<int>(syscall(SYS_io_submit, ctx, nr, iocbs));
}

static inline int sys_io_getevents(aio_context_t ctx, long min_nr,
                                   long max_nr, struct io_event* events,
                                   struct timespec* timeout) {
    return static_cast<int>(
        syscall(SYS_io_getevents, ctx, min_nr, max_nr, events, timeout));
}

struct AioRequest {
    struct iocb cb;
    ssize_t result;
    std::coroutine_handle<> handle;
};

struct AioSubmitAwaitable {
    AioRequest* req;
    aio_context_t ctx;

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        req->handle = h;
        struct iocb* cbs[1] = {&req->cb};
        int rc = sys_io_submit(ctx, 1, cbs);
        if (rc != 1) {
            req->result = (rc < 0) ? -errno : -EIO;
            return false;
        }
        return true;
    }

    ssize_t await_resume() noexcept { return req->result; }
};

AioIO::~AioIO() { close(); }

int AioIO::open(const char* path, size_t size) {
    fd_ = ::open(path, O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) return -errno;

    struct stat st;
    if (::fstat(fd_, &st) < 0) {
        int err = errno;
        ::close(fd_);
        fd_ = -1;
        return -err;
    }

    if (static_cast<size_t>(st.st_size) < size) {
        if (::ftruncate(fd_, static_cast<off_t>(size)) < 0) {
            int err = errno;
            ::close(fd_);
            fd_ = -1;
            return -err;
        }
    }

    size_ = size;

    ctx_ = 0;
    if (sys_io_setup(256, &ctx_) < 0) {
        int err = errno;
        ::close(fd_);
        fd_ = -1;
        return -err;
    }

    running_.store(true, std::memory_order_relaxed);
    poller_ = std::thread(&AioIO::poller_loop, this);
    return 0;
}

void AioIO::close() {
    if (running_.load(std::memory_order_relaxed)) {
        running_.store(false, std::memory_order_release);
        if (poller_.joinable())
            poller_.join();
    }

    if (ctx_ != 0) {
        sys_io_destroy(ctx_);
        ctx_ = 0;
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

CoroTask<ssize_t> AioIO::pread(void* buf, size_t count, off_t offset) {
    AioRequest req{};
    std::memset(&req.cb, 0, sizeof(req.cb));
    req.cb.aio_fildes = static_cast<uint32_t>(fd_);
    req.cb.aio_lio_opcode = IOCB_CMD_PREAD;
    req.cb.aio_buf = reinterpret_cast<uint64_t>(buf);
    req.cb.aio_nbytes = count;
    req.cb.aio_offset = offset;
    req.cb.aio_data = reinterpret_cast<uint64_t>(&req);

    ssize_t result = co_await AioSubmitAwaitable{&req, ctx_};
    co_return result;
}

CoroTask<ssize_t> AioIO::pwrite(const void* buf, size_t count, off_t offset) {
    AioRequest req{};
    std::memset(&req.cb, 0, sizeof(req.cb));
    req.cb.aio_fildes = static_cast<uint32_t>(fd_);
    req.cb.aio_lio_opcode = IOCB_CMD_PWRITE;
    req.cb.aio_buf = reinterpret_cast<uint64_t>(buf);
    req.cb.aio_nbytes = count;
    req.cb.aio_offset = offset;
    req.cb.aio_data = reinterpret_cast<uint64_t>(&req);

    ssize_t result = co_await AioSubmitAwaitable{&req, ctx_};
    co_return result;
}

IoBuffer AioIO::alloc_buffer(size_t size) {
    size_t aligned = (size + 511) & ~size_t{511};
    return IoBuffer::alloc_aligned(aligned, 512);
}

void AioIO::poller_loop() {
    static constexpr int kMaxEvents = 64;
    struct io_event events[kMaxEvents];

    while (running_.load(std::memory_order_acquire)) {
        struct timespec timeout = {0, 100'000'000};
        int n = sys_io_getevents(ctx_, 1, kMaxEvents, events, &timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            auto* req = reinterpret_cast<AioRequest*>(events[i].data);
            req->result = static_cast<ssize_t>(events[i].res);
            req->handle.resume();
        }
    }

    for (;;) {
        struct timespec timeout = {0, 0};
        int n = sys_io_getevents(ctx_, 0, kMaxEvents, events, &timeout);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            auto* req = reinterpret_cast<AioRequest*>(events[i].data);
            req->result = static_cast<ssize_t>(events[i].res);
            req->handle.resume();
        }
    }
}

}  // namespace udepot
