// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

#include <spdk/nvme.h>
#include <spdk/env.h>
#include <rte_malloc.h>

#include "udepot/buffer.h"
#include "udepot/coro.h"

namespace udepot {

struct SpdkNamespace {
    struct spdk_nvme_ctrlr* ctlr = nullptr;
    struct spdk_nvme_ns* ns = nullptr;
    char name[64] = {};

    uint32_t get_sector_size() const {
        return spdk_nvme_ns_get_sector_size(ns);
    }
    uint64_t get_num_sectors() const {
        return spdk_nvme_ns_get_num_sectors(ns);
    }
    uint64_t get_size() const {
        return spdk_nvme_ns_get_size(ns);
    }
};

struct NvmefTarget {
    enum Transport { kTcp, kRdma };
    Transport transport = kTcp;
    std::string traddr;
    std::string trsvcid;
    std::string subnqn;
};

struct SpdkController {
    struct spdk_nvme_ctrlr* ctlr = nullptr;
    char name[64] = {};
};

class SpdkGlobalState {
public:
    SpdkGlobalState() = default;
    ~SpdkGlobalState();

    SpdkGlobalState(const SpdkGlobalState&) = delete;
    SpdkGlobalState& operator=(const SpdkGlobalState&) = delete;

    void add_nvmef_target(NvmefTarget target);
    int init();
    void shutdown();

    void register_ctrlr(struct spdk_nvme_ctrlr* ctlr);
    void register_ns(struct spdk_nvme_ctrlr* ctlr, struct spdk_nvme_ns* ns);

    const std::vector<SpdkNamespace>& namespaces() const { return namespaces_; }
    bool initialized() const { return initialized_; }

private:
    std::vector<SpdkController> controllers_;
    std::vector<SpdkNamespace> namespaces_;
    std::vector<NvmefTarget> nvmef_targets_;
    bool initialized_ = false;

    int register_controllers();
    void unregister_controllers();
    int probe_nvmef_target(const NvmefTarget& target);
};

struct SpdkQpair {
    SpdkNamespace* ns = nullptr;
    struct spdk_nvme_qpair* qpair = nullptr;
    size_t npending = 0;

    SpdkQpair() = default;
    SpdkQpair(SpdkNamespace* namespace_ptr);
    ~SpdkQpair();

    SpdkQpair(const SpdkQpair&) = delete;
    SpdkQpair& operator=(const SpdkQpair&) = delete;
    SpdkQpair(SpdkQpair&& o) noexcept;
    SpdkQpair& operator=(SpdkQpair&&) = delete;

    uint32_t get_sector_size() const { return ns->get_sector_size(); }
    uint64_t get_size() const { return ns->get_size(); }

    int submit_read(void* buf, uint64_t lba, uint32_t lba_cnt,
                    spdk_nvme_cmd_cb cb_fn, void* cb_arg);
    int submit_write(void* buf, uint64_t lba, uint32_t lba_cnt,
                     spdk_nvme_cmd_cb cb_fn, void* cb_arg);

    int32_t execute_completions(uint32_t max_completions = 0);

    void process_admin_completions();

    void* alloc_dma_buffer(size_t size);
    void free_dma_buffer(void* ptr);
};

// SPDK NVMe I/O backend for uDepot-ng.
//
// Matches the IoBackend concept. Uses SPDK's userspace NVMe driver for
// direct device access. Per-thread queue pairs, DMA buffer bounce for
// pread/pwrite, and a poller thread for completion processing.
class SpdkIO {
public:
    SpdkIO() noexcept = default;
    ~SpdkIO();

    SpdkIO(const SpdkIO&) = delete;
    SpdkIO& operator=(const SpdkIO&) = delete;

    // One-time global SPDK initialization. Must be called before any
    // SpdkIO instance is opened. Parses UDEPOT_NVMEF env var for NVMeoF
    // targets, initializes SPDK environment, and probes controllers.
    static int global_init();
    static void global_shutdown();

    // Add an NVMe-oF target before calling global_init().
    enum class NvmefTransport { kTcp, kRdma };
    static void add_nvmef_target(NvmefTransport transport,
                                 const std::string& traddr,
                                 const std::string& trsvcid,
                                 const std::string& subnqn);

    int open(const char* path, size_t size);
    void close();

    CoroTask<ssize_t> pread(void* buf, size_t count, off_t offset);
    CoroTask<ssize_t> pwrite(const void* buf, size_t count, off_t offset);

    size_t get_size() const noexcept { return size_; }
    IoBuffer alloc_buffer(size_t size);

private:
    size_t size_ = 0;
    std::thread poller_;
    std::atomic<bool> running_{false};

    static SpdkGlobalState global_state_;
    static std::string namespace_name_;

    SpdkQpair* get_thread_qpair();

    void poller_loop();
};

}  // namespace udepot
