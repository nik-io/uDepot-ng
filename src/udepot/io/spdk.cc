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
#include <string>

#include <sched.h>

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

// The cpu set as a hex mask ("0x..."), as DPDK's -c option takes it.
static std::string cpuset_to_mask(const cpu_set_t& set) {
    std::string hex;
    for (int base = 0; base < CPU_SETSIZE; base += 4) {
        int nibble = 0;
        for (int b = 0; b < 4; ++b)
            if (CPU_ISSET(base + b, &set)) nibble |= 1 << b;
        hex.insert(hex.begin(), "0123456789abcdef"[nibble]);
    }
    size_t first = hex.find_first_not_of('0');
    return "0x" + (first == std::string::npos ? "0" : hex.substr(first));
}

static int spdk_env_init_once() {
    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "udepot_ng";
    opts.shm_id = -1;
    // As uDepot's spdk_init: our affinity is SPDK's core mask, so the EAL
    // claims no core outside the process's cpu set (by default it takes
    // core 0, where a co-located target's reactor may be spinning).
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
        perror("sched_getaffinity");
        return -1;
    }
    std::string core_mask = cpuset_to_mask(affinity);
    opts.core_mask = core_mask.c_str();
    // Hosts without hugepages (CI, containers) can still reach an NVMe-oF
    // TCP target, which needs no device DMA.
    const char* no_huge = getenv("UDEPOT_SPDK_NO_HUGE");
    if (no_huge && no_huge[0] == '1') {
        opts.no_huge = true;
        opts.mem_size = 1024;
        opts.iova_mode = "va";
    }
    int rc = spdk_env_init(&opts);
    if (rc) {
        fprintf(stderr, "spdk_env_init() failed: %d\n", rc);
        return -1;
    }
    // The EAL pins the calling thread to its main core. uDepot's TRT
    // pinned the threads it owned; uDepot-ng runs on its caller's threads,
    // so give the caller its own affinity back (threads it creates later
    // inherit it).
    if (sched_setaffinity(0, sizeof(affinity), &affinity) != 0)
        perror("sched_setaffinity");
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

void SpdkGlobalState::register_qpair(SpdkQpair* qp) {
    std::lock_guard<std::mutex> lock(qpairs_mu_);
    qpairs_.push_back(qp);
}

void SpdkGlobalState::unregister_qpair(SpdkQpair* qp) {
    std::lock_guard<std::mutex> lock(qpairs_mu_);
    qpairs_.erase(std::remove(qpairs_.begin(), qpairs_.end(), qp),
                  qpairs_.end());
}

void SpdkGlobalState::unregister_controllers() {
    // Queue pairs live in their threads' TLS and may outlive this; free
    // them here, before their controllers go, and leave the TLS objects
    // empty. No I/O may be in flight (callers close their stores first).
    {
        std::lock_guard<std::mutex> lock(qpairs_mu_);
        for (SpdkQpair* qp : qpairs_) {
            if (qp->qpair) spdk_nvme_ctrlr_free_io_qpair(qp->qpair);
            qp->qpair = nullptr;
        }
        qpairs_.clear();
    }
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

SpdkQpair::SpdkQpair(SpdkNamespace* namespace_ptr, SpdkGlobalState* owner,
                     unsigned queue_depth)
    : ns(namespace_ptr), gs(owner) {
    struct spdk_nvme_io_qpair_opts opts;
    spdk_nvme_ctrlr_get_default_io_qpair_opts(ns->ctlr, &opts, sizeof(opts));
    // A depth beyond SPDK's default gets a bigger request pool, never a
    // smaller one: an I/O larger than the transfer limit is split into
    // child requests from the same pool, so a pool sized to the caller's
    // depth alone could never take a large value, or the store's own 4 MiB
    // reads, and they would be refused forever (PR #4 review). Requests
    // beyond the device queue are queued by SPDK itself, so the device
    // queue keeps SPDK's size (which must be at least 2).
    if (queue_depth > opts.io_queue_requests)
        opts.io_queue_requests = queue_depth;
    qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns->ctlr, &opts, sizeof(opts));
    if (!qpair) {
        fprintf(stderr, "spdk: queue pair allocation failed\n");
        abort();
    }
    if (gs) gs->register_qpair(this);
}

SpdkQpair::~SpdkQpair() {
    // Unregister first: shutdown() frees registered queue pairs itself.
    if (gs) gs->unregister_qpair(this);
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
      npending(std::exchange(o.npending, 0)),
      ready(std::move(o.ready)),
      gs(std::exchange(o.gs, nullptr)),
      bounced(std::exchange(o.bounced, 0)) {
    if (gs) {
        gs->unregister_qpair(&o);
        gs->register_qpair(this);
    }
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
    std::coroutine_handle<> handle;  // async: resume when complete
    bool* done;                      // sync: set when complete
    void* buf;                       // DMA memory
    uint64_t lba;
    uint32_t lba_cnt;
    bool write;
};

static void spdk_io_cb(void* ctx, const struct spdk_nvme_cpl* cpl) {
    auto* req = static_cast<SpdkRequest*>(ctx);
    assert(req->qp->npending > 0);
    --req->qp->npending;
    req->result = spdk_nvme_cpl_is_error(cpl) ? -EIO : 0;
    if (req->handle)
        req->qp->ready.push_back(req->handle);
    else
        *req->done = true;
}

int SpdkQpair::submit(SpdkRequest* req) {
    req->qp = this;
    int err = req->write
        ? spdk_nvme_ns_cmd_write(ns->ns, qpair, req->buf, req->lba,
                                 req->lba_cnt, spdk_io_cb, req, 0)
        : spdk_nvme_ns_cmd_read(ns->ns, qpair, req->buf, req->lba,
                                req->lba_cnt, spdk_io_cb, req, 0);
    if (err == 0) {
        ++npending;
        return 0;
    }
    if (err == -ENOMEM) return -EAGAIN;  // every request taken
    fprintf(stderr, "spdk: submitting %s request failed err=%d\n",
            req->write ? "write" : "read", err);
    return err;
}

// Submit an NVMe command on the calling thread's queue pair and suspend;
// the thread's poll resumes the coroutine when the command completes.
struct SpdkSubmitAwaitable {
    SpdkRequest* req;

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        req->handle = h;
        int rc = req->qp->submit(req);
        if (rc != 0) {
            req->result = rc < 0 ? rc : -EIO;
            return false;
        }
        return true;
    }

    ssize_t await_resume() noexcept { return req->result; }
};

// ─────────────────────────────────────────────────────────────────────────────
// SpdkIO — SPDK I/O backend
// ─────────────────────────────────────────────────────────────────────────────

SpdkGlobalState SpdkIO::global_state_;
std::string SpdkIO::namespace_name_;
std::atomic<unsigned> SpdkIO::new_qpair_depth_{0};

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

    new_qpair_depth_.store(queue_depth_, std::memory_order_relaxed);

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

    thread_qpair_ = std::make_unique<SpdkQpair>(
        target, &global_state_,
        new_qpair_depth_.load(std::memory_order_relaxed));
    set_thread_poll(&SpdkIO::poll_thread_qpair);
    return thread_qpair_.get();
}

bool SpdkIO::poll_thread_qpair() {
    SpdkQpair* qp = thread_qpair_.get();
    if (!qp || !qp->qpair) return false;
    qp->execute_completions(0);
    // Resumed coroutines may submit more and complete more; drain until
    // this pass found nothing ready.
    while (!qp->ready.empty()) {
        std::vector<std::coroutine_handle<>> ready;
        ready.swap(qp->ready);
        for (auto h : ready) h.resume();
    }
    return qp->npending > 0;
}

// Whether the device can transfer straight to or from buf: memory SPDK can
// translate (alloc_buffer's DMA memory), dword-aligned as NVMe requires, and
// covering whole sectors. SPDK registers memory in 2 MiB units, so checking
// each 2 MiB step and the last byte covers the whole range.
static bool direct_io_ok(const void* buf, size_t count, off_t offset,
                         uint32_t bsize) {
    if (count == 0 || count % bsize != 0 ||
        static_cast<uint64_t>(offset) % bsize != 0 ||
        reinterpret_cast<uintptr_t>(buf) % 4 != 0)
        return false;
    constexpr size_t kStep = 2 * 1024 * 1024;
    const char* p = static_cast<const char*>(buf);
    for (size_t off = 0; off < count; off += kStep)
        if (spdk_vtophys(p + off, nullptr) == SPDK_VTOPHYS_ERROR) return false;
    return spdk_vtophys(p + count - 1, nullptr) != SPDK_VTOPHYS_ERROR;
}

uint64_t SpdkIO::thread_bounce_count() {
    SpdkQpair* qp = thread_qpair_.get();
    return qp ? qp->bounced : 0;
}

ssize_t SpdkIO::pwrite_sync(const void* buf, size_t count, off_t offset) {
    SpdkQpair* qp = get_thread_qpair();
    if (!qp) return -EIO;

    uint32_t bsize = qp->get_sector_size();
    uint64_t lba_start = static_cast<uint64_t>(offset) / bsize;
    uint64_t lba_end = (static_cast<uint64_t>(offset) + count + bsize - 1) / bsize;
    uint64_t nlbas = lba_end - lba_start;

    void* dma_buf = nullptr;
    if (direct_io_ok(buf, count, offset, bsize)) {
        dma_buf = const_cast<void*>(buf);
    } else {
        dma_buf = qp->alloc_dma_buffer(nlbas * bsize);
        if (!dma_buf) return -ENOMEM;
        ++qp->bounced;
        size_t copy_off = static_cast<size_t>(offset) - lba_start * bsize;
        std::memcpy(static_cast<char*>(dma_buf) + copy_off, buf, count);
    }

    bool done = false;
    SpdkRequest req{qp, 0, {}, &done, dma_buf, lba_start,
                    static_cast<uint32_t>(nlbas), true};
    // uDepot's own metadata write: a queue pair full of this thread's I/O
    // frees a request as soon as one completes, so poll and retry rather
    // than fail the write.
    int rc;
    while ((rc = qp->submit(&req)) == -EAGAIN && qp->npending > 0)
        qp->execute_completions(0);
    if (rc == 0) {
        // Completions of this thread's suspended coroutines that arrive
        // meanwhile are queued for its next poll, not resumed here.
        while (!done) qp->execute_completions(0);
    }
    if (dma_buf != buf) qp->free_dma_buffer(dma_buf);
    if (rc != 0 || req.result < 0) return -EIO;
    return static_cast<ssize_t>(count);
}

CoroTask<ssize_t> SpdkIO::pread(void* buf, size_t count, off_t offset) {
    SpdkQpair* qp = get_thread_qpair();
    if (!qp) co_return -EIO;

    uint32_t bsize = qp->get_sector_size();
    uint64_t lba_start = static_cast<uint64_t>(offset) / bsize;
    uint64_t lba_end = (static_cast<uint64_t>(offset) + count + bsize - 1) / bsize;
    uint64_t nlbas = lba_end - lba_start;

    if (direct_io_ok(buf, count, offset, bsize)) {
        SpdkRequest req{qp, 0, {}, nullptr, buf, lba_start,
                        static_cast<uint32_t>(nlbas), false};
        ssize_t result = co_await SpdkSubmitAwaitable{&req};
        if (result < 0) co_return result;
        co_return static_cast<ssize_t>(count);
    }

    void* dma_buf = qp->alloc_dma_buffer(nlbas * bsize);
    if (!dma_buf) co_return -ENOMEM;
    ++qp->bounced;

    SpdkRequest req{qp, 0, {}, nullptr, dma_buf, lba_start,
                    static_cast<uint32_t>(nlbas), false};
    ssize_t result = co_await SpdkSubmitAwaitable{&req};

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

    void* dma_buf = nullptr;
    if (direct_io_ok(buf, count, offset, bsize)) {
        dma_buf = const_cast<void*>(buf);
    } else {
        dma_buf = qp->alloc_dma_buffer(nlbas * bsize);
        if (!dma_buf) co_return -ENOMEM;
        ++qp->bounced;
        size_t copy_off = static_cast<size_t>(offset) - lba_start * bsize;
        std::memcpy(static_cast<char*>(dma_buf) + copy_off, buf, count);
    }

    SpdkRequest req{qp, 0, {}, nullptr, dma_buf, lba_start,
                    static_cast<uint32_t>(nlbas), true};
    ssize_t result = co_await SpdkSubmitAwaitable{&req};

    if (dma_buf != buf) qp->free_dma_buffer(dma_buf);
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
