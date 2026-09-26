// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "udepot/io/spdk.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdlib>
#include <cstring>

#include <spdk/nvme.h>
#include <spdk/env.h>
#include <rte_config.h>
#include <rte_malloc.h>

namespace udepot {

// ─────────────────────────────────────────────────────────────────────────────
// SpdkGlobalState — SPDK environment init, controller probing, NVMeoF
// Ported from uDepot's trt/src/trt_util/spdk.cc
// ─────────────────────────────────────────────────────────────────────────────

static bool probe_cb(void* cb_ctx,
                     const struct spdk_nvme_transport_id* trid,
                     struct spdk_nvme_ctrlr_opts* opts) {
    (void)cb_ctx;
    (void)opts;
    if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
        fprintf(stderr, "Attaching to NVMe over Fabrics controller at %s:%s: %s\n",
                trid->traddr, trid->trsvcid, trid->subnqn);
    } else {
        fprintf(stderr, "Attaching to NVMe Controller at %s\n", trid->traddr);
    }
    return true;
}

static void attach_cb(void* cb_ctx,
                      const struct spdk_nvme_transport_id* trid,
                      struct spdk_nvme_ctrlr* ctrlr,
                      const struct spdk_nvme_ctrlr_opts* opts) {
    (void)opts;
    if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
        fprintf(stderr, "Attached to NVMe over Fabrics controller at %s:%s: %s\n",
                trid->traddr, trid->trsvcid, trid->subnqn);
    } else {
        fprintf(stderr, "Attached to NVMe Controller at %s\n", trid->traddr);
    }
    auto* gs = static_cast<SpdkGlobalState*>(cb_ctx);
    gs->register_ctrlr(ctrlr);
}

SpdkGlobalState::~SpdkGlobalState() {
    shutdown();
}

void SpdkGlobalState::add_nvmef_target(NvmefTarget target) {
    nvmef_targets_.push_back(std::move(target));
}

void SpdkGlobalState::register_ctrlr(struct spdk_nvme_ctrlr* ctlr) {
    const struct spdk_nvme_ctrlr_data* cdata = spdk_nvme_ctrlr_get_data(ctlr);

    SpdkController c{};
    c.ctlr = ctlr;
    snprintf(c.name, sizeof(c.name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
    controllers_.push_back(c);

    int num_ns = spdk_nvme_ctrlr_get_num_ns(ctlr);
    for (int nsid = 1; nsid <= num_ns; ++nsid) {
        register_ns(ctlr, spdk_nvme_ctrlr_get_ns(ctlr, nsid));
    }
}

void SpdkGlobalState::register_ns(struct spdk_nvme_ctrlr* ctlr,
                                   struct spdk_nvme_ns* ns) {
    if (!spdk_nvme_ns_is_active(ns))
        return;

    const struct spdk_nvme_ctrlr_data* cdata = spdk_nvme_ctrlr_get_data(ctlr);

    SpdkNamespace entry{};
    entry.ctlr = ctlr;
    entry.ns = ns;
    snprintf(entry.name, sizeof(entry.name), "%-20.20s (%-20.20s)",
             cdata->mn, cdata->sn);
    fprintf(stderr, "adding namespace: %s\n", entry.name);
    namespaces_.push_back(entry);
}

static int spdk_env_init_once() {
    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "udepot_ng";
    opts.shm_id = -1;
    int rc = spdk_env_init(&opts);
    if (rc) {
        fprintf(stderr, "spdk_env_init() failed: %d\n", rc);
        return -1;
    }
    return 0;
}

int SpdkGlobalState::register_controllers() {
    if (spdk_env_init_once() != 0)
        return -1;

    // A failing local PCIe probe is not fatal when NVMeoF targets are
    // configured: a host with no local NVMe (or without VFIO/UIO bound)
    // still has to be able to reach a fabrics target.
    const bool pcie_ok =
        spdk_nvme_probe(nullptr, this, probe_cb, attach_cb, nullptr) == 0;
    if (!pcie_ok) {
        if (nvmef_targets_.empty()) {
            fprintf(stderr, "spdk_nvme_probe() failed\n");
            return -1;
        }
        fprintf(stderr,
                "local PCIe probe found nothing; continuing with %zu NVMeoF target(s)\n",
                nvmef_targets_.size());
    }

    for (const auto& target : nvmef_targets_) {
        int err = probe_nvmef_target(target);
        if (err) {
            fprintf(stderr, "Failed to connect to NVMeoF target %s:%s\n",
                    target.traddr.c_str(), target.trsvcid.c_str());
        }
    }

    if (namespaces_.empty()) {
        fprintf(stderr, "No NVMe namespaces found (local or fabrics)\n");
        return -1;
    }

    return 0;
}

int SpdkGlobalState::probe_nvmef_target(const NvmefTarget& target) {
    struct spdk_nvme_transport_id trid = {};

    switch (target.transport) {
        case NvmefTarget::kTcp:
            trid.trtype = SPDK_NVME_TRANSPORT_TCP;
            break;
        case NvmefTarget::kRdma:
            trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
            break;
    }

    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    snprintf(trid.traddr, sizeof(trid.traddr), "%s", target.traddr.c_str());
    snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", target.trsvcid.c_str());
    snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", target.subnqn.c_str());

    fprintf(stderr, "Connecting to NVMeoF target at %s:%s (%s)\n",
            trid.traddr, trid.trsvcid, trid.subnqn);

    if (spdk_nvme_probe(&trid, this, probe_cb, attach_cb, nullptr) != 0) {
        fprintf(stderr, "spdk_nvme_probe() failed for NVMeoF target %s:%s\n",
                trid.traddr, trid.trsvcid);
        return -1;
    }
    return 0;
}

int SpdkGlobalState::init() {
    if (initialized_)
        return 0;
    int rc = register_controllers();
    if (rc < 0)
        return rc;
    initialized_ = true;
    return 0;
}

void SpdkGlobalState::shutdown() {
    if (!initialized_)
        return;
    unregister_controllers();
    initialized_ = false;
}

void SpdkGlobalState::unregister_controllers() {
    namespaces_.clear();
    for (auto& c : controllers_) {
        spdk_nvme_detach(c.ctlr);
    }
    controllers_.clear();
}

void SpdkGlobalState::process_all_admin_completions() {
    for (auto& c : controllers_) {
        spdk_nvme_ctrlr_process_admin_completions(c.ctlr);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// SpdkQpair — per-thread NVMe queue pair
// Ported from uDepot's trt/src/trt_util/spdk.hh
// ─────────────────────────────────────────────────────────────────────────────

SpdkQpair::SpdkQpair(SpdkNamespace* namespace_ptr) : ns(namespace_ptr) {
    qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns->ctlr, nullptr, 0);
    if (!qpair) {
        fprintf(stderr, "spdk: queue pair allocation failed\n");
        abort();
    }
}

SpdkQpair::~SpdkQpair() {
    if (qpair) {
        int err = spdk_nvme_ctrlr_free_io_qpair(qpair);
        if (err)
            fprintf(stderr, "spdk_nvme_ctrlr_free_io_qpair() failed: %d\n", err);
        qpair = nullptr;
    }
}

SpdkQpair::SpdkQpair(SpdkQpair&& o) noexcept
    : ns(std::exchange(o.ns, nullptr)),
      qpair(std::exchange(o.qpair, nullptr)),
      npending(std::exchange(o.npending, 0)) {}

int SpdkQpair::submit_read(void* buf, uint64_t lba, uint32_t lba_cnt,
                            spdk_nvme_cmd_cb cb_fn, void* cb_arg) {
    int err = spdk_nvme_ns_cmd_read(ns->ns, qpair, buf, lba, lba_cnt,
                                    cb_fn, cb_arg, 0);
    if (err) {
        fprintf(stderr, "spdk: submitting read request failed err=%d\n", err);
        return err;
    }
    ++npending;
    return 0;
}

int SpdkQpair::submit_write(void* buf, uint64_t lba, uint32_t lba_cnt,
                             spdk_nvme_cmd_cb cb_fn, void* cb_arg) {
    int err = spdk_nvme_ns_cmd_write(ns->ns, qpair, buf, lba, lba_cnt,
                                     cb_fn, cb_arg, 0);
    if (err) {
        fprintf(stderr, "spdk: submitting write request failed err=%d\n", err);
        return err;
    }
    ++npending;
    return 0;
}

int32_t SpdkQpair::execute_completions(uint32_t max_completions) {
    if (npending == 0)
        return 0;
    int32_t r = spdk_nvme_qpair_process_completions(qpair, max_completions);
    if (r < 0) {
        fprintf(stderr, "spdk_nvme_qpair_process_completions returned error. Aborting\n");
        abort();
    }
    return r;
}

void SpdkQpair::process_admin_completions() {
    spdk_nvme_ctrlr_process_admin_completions(ns->ctlr);
}

void* SpdkQpair::alloc_dma_buffer(size_t size) {
    uint32_t sector_size = get_sector_size();
    return rte_malloc_socket(nullptr, size, sector_size, SOCKET_ID_ANY);
}

void SpdkQpair::free_dma_buffer(void* ptr) {
    rte_free(ptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// SpdkRequest + SpdkSubmitAwaitable — coroutine integration
// Same pattern as AioRequest + AioSubmitAwaitable
// ─────────────────────────────────────────────────────────────────────────────

struct SpdkRequest {
    SpdkQpair* qp;
    ssize_t result;
    bool completed;
};

static void spdk_io_cb(void* ctx, const struct spdk_nvme_cpl* cpl) {
    auto* req = static_cast<SpdkRequest*>(ctx);
    assert(req->qp->npending > 0);
    --req->qp->npending;
    req->result = spdk_nvme_cpl_is_error(cpl) ? -EIO : 0;
    req->completed = true;
}

// Submit an NVMe command and poll completions inline on the calling
// thread's qpair until it completes.  The coroutine never actually
// suspends — this matches uDepot's read_sync/write_sync pattern where
// each thread submits and polls its own per-thread qpair.
struct SpdkSubmitAwaitable {
    enum class Op { kRead, kWrite };

    SpdkRequest* req;
    SpdkQpair* qp;
    void* dma_buf;
    uint64_t lba;
    uint32_t lba_cnt;
    Op op;

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<>) noexcept {
        req->qp = qp;
        req->completed = false;
        int rc;
        if (op == Op::kRead)
            rc = qp->submit_read(dma_buf, lba, lba_cnt, spdk_io_cb, req);
        else
            rc = qp->submit_write(dma_buf, lba, lba_cnt, spdk_io_cb, req);
        if (rc != 0) {
            req->result = -EIO;
            return false;
        }
        while (!req->completed)
            qp->execute_completions(0);
        return false;
    }

    ssize_t await_resume() noexcept { return req->result; }
};

// ─────────────────────────────────────────────────────────────────────────────
// SpdkIO — SPDK I/O backend
// ─────────────────────────────────────────────────────────────────────────────

SpdkGlobalState SpdkIO::global_state_;
std::string SpdkIO::namespace_name_;

// Per-thread queue pair — lazily initialized on first use.
static thread_local std::unique_ptr<SpdkQpair> thread_qpair_;

// Parse the UDEPOT_NVMEF environment variable to register NVMe-oF targets.
// Format: one or more TCP targets, ';'-separated, each traddr:trsvcid:subnqn.
// The subnqn itself contains ':' (e.g. nqn.2016-06.io.spdk:cnode1), so each
// entry is split on its first two ':' only and the remainder is the nqn.
static void add_env_nvmef_targets() {
    const char* env = getenv("UDEPOT_NVMEF");
    if (env == nullptr || env[0] == '\0')
        return;

    const std::string spec(env);
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t sep = spec.find(';', pos);
        const std::string entry =
            spec.substr(pos, sep == std::string::npos ? std::string::npos
                                                       : sep - pos);
        pos = (sep == std::string::npos) ? spec.size() : sep + 1;
        if (entry.empty())
            continue;

        const size_t c1 = entry.find(':');
        const size_t c2 = entry.find(':', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) {
            fprintf(stderr, "UDEPOT_NVMEF entry '%s' is not traddr:trsvcid:subnqn; ignoring\n",
                    entry.c_str());
            continue;
        }
        const std::string traddr = entry.substr(0, c1);
        const std::string trsvcid = entry.substr(c1 + 1, c2 - c1 - 1);
        const std::string subnqn = entry.substr(c2 + 1);
        fprintf(stderr, "UDEPOT_NVMEF: adding TCP target %s:%s (%s)\n",
                traddr.c_str(), trsvcid.c_str(), subnqn.c_str());
        SpdkIO::add_nvmef_target(SpdkIO::NvmefTransport::kTcp,
                                 traddr, trsvcid, subnqn);
    }
}

void SpdkIO::add_nvmef_target(NvmefTransport transport,
                               const std::string& traddr,
                               const std::string& trsvcid,
                               const std::string& subnqn) {
    NvmefTarget t;
    t.transport = (transport == NvmefTransport::kRdma) ? NvmefTarget::kRdma
                                                        : NvmefTarget::kTcp;
    t.traddr = traddr;
    t.trsvcid = trsvcid;
    t.subnqn = subnqn;
    global_state_.add_nvmef_target(std::move(t));
}

int SpdkIO::global_init() {
    add_env_nvmef_targets();
    return global_state_.init();
}

void SpdkIO::global_shutdown() {
    global_state_.shutdown();
}

SpdkIO::~SpdkIO() {
    close();
}

int SpdkIO::open(const char* path, size_t size) {
    if (!global_state_.initialized()) {
        fprintf(stderr, "SpdkIO::open: global_init() not called\n");
        return -EINVAL;
    }

    // Select the namespace matching the path (or first available).
    const auto& nss = global_state_.namespaces();
    if (nss.empty())
        return -ENODEV;

    if (namespace_name_.empty()) {
        auto match = std::find_if(nss.begin(), nss.end(),
            [path](const SpdkNamespace& ns) {
                return std::string(ns.name).find(path) != std::string::npos;
            });
        if (match != nss.end()) {
            namespace_name_ = match->name;
        } else {
            namespace_name_ = nss.front().name;
        }
        fprintf(stderr, "SpdkIO: using namespace: %s\n", namespace_name_.c_str());
    }

    // Use the device size from the namespace, ignoring the file-based size.
    // The caller passes size 0 for block devices (standard uDepot convention).
    SpdkQpair* qp = get_thread_qpair();
    if (!qp)
        return -EIO;

    size_ = (size > 0) ? size : qp->get_size();

    running_.store(true, std::memory_order_relaxed);
    poller_ = std::thread(&SpdkIO::poller_loop, this);
    return 0;
}

void SpdkIO::close() {
    if (running_.load(std::memory_order_relaxed)) {
        running_.store(false, std::memory_order_release);
        if (poller_.joinable())
            poller_.join();
    }

    // The thread_local qpairs are cleaned up by their own destructors.
    size_ = 0;
}

SpdkQpair* SpdkIO::get_thread_qpair() {
    if (thread_qpair_)
        return thread_qpair_.get();

    const auto& nss = global_state_.namespaces();
    SpdkNamespace* target = nullptr;
    for (auto& ns : nss) {
        if (std::string(ns.name) == namespace_name_ ||
            namespace_name_.empty()) {
            target = const_cast<SpdkNamespace*>(&ns);
            break;
        }
    }
    if (!target) {
        fprintf(stderr, "SpdkIO: namespace '%s' not found\n",
                namespace_name_.c_str());
        return nullptr;
    }

    thread_qpair_ = std::make_unique<SpdkQpair>(target);
    return thread_qpair_.get();
}

CoroTask<ssize_t> SpdkIO::pread(void* buf, size_t count, off_t offset) {
    SpdkQpair* qp = get_thread_qpair();
    if (!qp) co_return -EIO;

    uint32_t bsize = qp->get_sector_size();
    uint64_t lba_start = static_cast<uint64_t>(offset) / bsize;
    uint64_t lba_end = (static_cast<uint64_t>(offset) + count + bsize - 1) / bsize;
    uint64_t nlbas = lba_end - lba_start;

    void* dma_buf = qp->alloc_dma_buffer(nlbas * bsize);
    if (!dma_buf) co_return -ENOMEM;

    SpdkRequest req{};
    ssize_t result = co_await SpdkSubmitAwaitable{
        &req, qp, dma_buf, lba_start, static_cast<uint32_t>(nlbas),
        SpdkSubmitAwaitable::Op::kRead
    };

    if (result < 0) {
        qp->free_dma_buffer(dma_buf);
        co_return result;
    }

    size_t copy_off = static_cast<size_t>(offset) - lba_start * bsize;
    std::memcpy(buf, static_cast<char*>(dma_buf) + copy_off, count);
    qp->free_dma_buffer(dma_buf);
    co_return static_cast<ssize_t>(count);
}

CoroTask<ssize_t> SpdkIO::pwrite(const void* buf, size_t count, off_t offset) {
    SpdkQpair* qp = get_thread_qpair();
    if (!qp) co_return -EIO;

    uint32_t bsize = qp->get_sector_size();
    uint64_t lba_start = static_cast<uint64_t>(offset) / bsize;
    uint64_t lba_end = (static_cast<uint64_t>(offset) + count + bsize - 1) / bsize;
    uint64_t nlbas = lba_end - lba_start;

    void* dma_buf = qp->alloc_dma_buffer(nlbas * bsize);
    if (!dma_buf) co_return -ENOMEM;

    size_t copy_off = static_cast<size_t>(offset) - lba_start * bsize;
    std::memcpy(static_cast<char*>(dma_buf) + copy_off, buf, count);

    SpdkRequest req{};
    ssize_t result = co_await SpdkSubmitAwaitable{
        &req, qp, dma_buf, lba_start, static_cast<uint32_t>(nlbas),
        SpdkSubmitAwaitable::Op::kWrite
    };

    qp->free_dma_buffer(dma_buf);
    if (result < 0) co_return result;
    co_return static_cast<ssize_t>(count);
}

IoBuffer SpdkIO::alloc_buffer(size_t size) {
    return IoBuffer::alloc_dma(size);
}

// Poller thread: drives admin queue polling for NVMeoF keep-alive.
// I/O completions are polled inline by each thread's SpdkSubmitAwaitable.
// Admin completions are throttled to ~10x/sec.
void SpdkIO::poller_loop() {
    uint64_t admin_interval_ticks = spdk_get_ticks_hz() / 10;
    uint64_t last_admin_tick = spdk_get_ticks();

    while (running_.load(std::memory_order_acquire)) {
        uint64_t now = spdk_get_ticks();
        if (now - last_admin_tick >= admin_interval_ticks) {
            global_state_.process_all_admin_completions();
            last_admin_tick = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace udepot
