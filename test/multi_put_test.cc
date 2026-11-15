// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

// udepot::multi_put (src/udepot/multi_put.h), the batch behind pyudepot's
// uDepotMultiPutBuf. It is a header the store library compiles, so the
// bindings being out of the TSAN build does not leave it unraced here.

#include "udepot/multi_put.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <sys/sysinfo.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "udepot/io/aio.h"

using udepot::AioIO;
using udepot::PutBuffer;
using udepot::StoreConfig;
using udepot::uDepot;
using Store = uDepot<AioIO>;

namespace {

constexpr unsigned kWindow = 8;
constexpr int kUnset = INT_MIN;
constexpr size_t kKeySize = 13;
constexpr size_t kValSize = 100;

std::string key_of(int i) {
    char b[32];
    snprintf(b, sizeof(b), "mput_key_%04d", i);
    return b;
}

// The value of key i written by the batch numbered gen.
std::string val_of(int i, int gen = 0) {
    std::string v(kValSize, static_cast<char>('a' + i % 26));
    char tag[24];
    int len = snprintf(tag, sizeof(tag), "v%07d_g%04d", i, gen);
    std::memcpy(v.data(), tag, len);
    return v;
}

class MultiPutTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("udepot_multi_put_test_" + std::to_string(getpid()));
        config_.path = path_.c_str();
        config_.size = 64 * 1024 * 1024;
        config_.grain_size = 512;
        config_.force_destroy = true;
        config_.queue_depth = kWindow;
        ASSERT_EQ(store_.open(config_), 0);
        ASSERT_EQ(store_.io().queue_depth(), kWindow);
    }

    void TearDown() override {
        store_.close();
        std::filesystem::remove(path_);
    }

    struct Batch {
        std::vector<std::string> keys;
        std::vector<const uint8_t*> key_ptrs;
        std::vector<uint32_t> key_sizes;
        std::vector<PutBuffer> bufs;
        std::vector<PutBuffer*> buf_ptrs;
        std::vector<int> results;
    };

    // A batch of puts: entry j stores val_of(ids[j], gen) under key_of(ids[j]).
    Batch make_batch(const std::vector<int>& ids, int gen = 0) {
        Batch b;
        const size_t n = ids.size();
        b.bufs.resize(n);
        b.results.assign(n, kUnset);
        for (size_t j = 0; j < n; ++j) {
            b.keys.push_back(key_of(ids[j]));
            b.bufs[j] = store_.alloc_put_buffer(kKeySize, kValSize);
            EXPECT_TRUE(b.bufs[j].valid());
            std::string v = val_of(ids[j], gen);
            std::memcpy(b.bufs[j].value().data(), v.data(), kValSize);
        }
        for (size_t j = 0; j < n; ++j) {
            b.key_ptrs.push_back(
                reinterpret_cast<const uint8_t*>(b.keys[j].data()));
            b.key_sizes.push_back(static_cast<uint32_t>(b.keys[j].size()));
            b.buf_ptrs.push_back(&b.bufs[j]);
        }
        return b;
    }

    static std::vector<int> iota(int n) {
        std::vector<int> v(n);
        for (int i = 0; i < n; ++i) v[i] = i;
        return v;
    }

    int run(Batch& b, unsigned window) {
        return udepot::multi_put(store_, static_cast<uint32_t>(b.keys.size()),
                                 b.key_ptrs.data(), b.key_sizes.data(),
                                 b.buf_ptrs.data(), b.results.data(), window);
    }

    // The value stored under key_of(i), or "" if it is missing.
    std::string read(Store& s, int i) {
        std::string k = key_of(i);
        uint8_t out[kValSize + 8];
        size_t got = 0;
        int rc = s.get(k, out, sizeof(out), &got).run_sync();
        if (rc != 0) return "";
        return std::string(reinterpret_cast<char*>(out), got);
    }

    std::filesystem::path path_;
    StoreConfig config_;
    Store store_;
};

}  // namespace

TEST_F(MultiPutTest, EveryKeyIsStoredWithItsOwnValue) {
    constexpr int kMax = 200;
    for (int n : {0, 1, 2, static_cast<int>(kWindow) - 1,
                  static_cast<int>(kWindow), static_cast<int>(kWindow) + 1,
                  4 * static_cast<int>(kWindow), kMax}) {
        SCOPED_TRACE("n=" + std::to_string(n));
        Batch b = make_batch(iota(n), /*gen=*/n);
        ASSERT_EQ(run(b, kWindow), 0);
        for (int i = 0; i < n; ++i) {
            EXPECT_EQ(b.results[i], 0) << i;
            EXPECT_EQ(read(store_, i), val_of(i, n)) << i;
        }
    }
}

// The puts were written, not just accepted: a clean close and reopen
// reads them back.
TEST_F(MultiPutTest, TheValuesSurviveAReopen) {
    Batch b = make_batch(iota(100));
    ASSERT_EQ(run(b, kWindow), 0);
    store_.close();
    config_.force_destroy = false;
    Store again;
    ASSERT_EQ(again.open(config_), 0);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(read(again, i), val_of(i)) << i;
    again.close();
}

// The window is what keeps a batch inside the queue: past it the backend
// refuses (-EAGAIN), and that is reported per key, not hidden. The kernel
// sizes an AIO context for at least 8 I/Os per possible CPU whatever depth
// was asked for (see multi_get_test.cc), so "past it" is that many. That a
// window of the queue's depth is clean and one past it is not is also the
// test that the puts overlap, up to the window: a batch that ran them one
// at a time would never find the queue full.
TEST_F(MultiPutTest, AWindowPastTheQueueFailsWithEagain) {
    const int n = std::max(4096, 16 * get_nprocs_conf() + 1024);
    Batch ok = make_batch(iota(n));
    ASSERT_EQ(run(ok, store_.io().queue_depth()), 0);
    for (int r : ok.results) EXPECT_EQ(r, 0);

    Batch over = make_batch(iota(n), 1);
    ASSERT_EQ(run(over, n), 0);
    int eagain = 0;
    for (int i = 0; i < n; ++i) {
        const int r = over.results[i];
        EXPECT_TRUE(r == 0 || r == -EAGAIN) << r;
        eagain += r == -EAGAIN;
        // A put that failed stored nothing of its batch: the key keeps the
        // earlier batch's value; one that succeeded holds this batch's.
        EXPECT_EQ(read(store_, i), val_of(i, r == 0 ? 1 : 0)) << i;
    }
    EXPECT_GT(eagain, 0) << "no put found the queue full";
}

// One failed put is reported at its index and does not stop the others.
TEST_F(MultiPutTest, AFailureIsReportedPerKeyAndTheRestAreStored) {
    constexpr int n = 40;
    Batch b = make_batch(iota(n));
    b.bufs[3] = PutBuffer();                       // not valid
    b.bufs[17] = store_.alloc_put_buffer(kKeySize + 1, kValSize);  // wrong key size
    b.key_sizes[29] = kKeySize - 1;                // key of another size than its buffer
    ASSERT_EQ(run(b, kWindow), 0);
    for (int i = 0; i < n; ++i) {
        if (i == 3 || i == 17 || i == 29) {
            EXPECT_EQ(b.results[i], -EINVAL) << i;
            EXPECT_EQ(read(store_, i), "") << i;
        } else {
            EXPECT_EQ(b.results[i], 0) << i;
            EXPECT_EQ(read(store_, i), val_of(i)) << i;
        }
    }
}

// Equal keys in one batch are stored in list order, as serial puts: the last
// one holds the key. The batch is wider than the window, with repeats at
// every distance.
TEST_F(MultiPutTest, TheLastOfEqualKeysWins) {
    std::vector<int> ids;
    for (int round = 0; round < 6; ++round)
        for (int k = 0; k < 20; ++k) ids.push_back(k);
    // Each entry of Batch has its own value: tell the rounds apart.
    Batch b = make_batch(ids);
    for (size_t j = 0; j < ids.size(); ++j) {
        std::string v = val_of(ids[j], static_cast<int>(j));
        std::memcpy(b.bufs[j].value().data(), v.data(), kValSize);
    }
    ASSERT_EQ(run(b, kWindow), 0);
    for (int r : b.results) EXPECT_EQ(r, 0);
    for (int k = 0; k < 20; ++k)
        EXPECT_EQ(read(store_, k),
                  val_of(k, static_cast<int>(ids.size()) - 20 + k)) << k;
}

// Puts of one key are not in flight together, whatever the window: every put
// of a batch of one key, with a window past the queue, finds room. Different
// keys are not held back by this (the window test above).
TEST_F(MultiPutTest, PutsOfOneKeyRunOneAtATime) {
    const int n = std::max(4096, 16 * get_nprocs_conf() + 1024);
    Batch b = make_batch(std::vector<int>(n, 7));
    for (int j = 0; j < n; ++j) {
        std::string v = val_of(7, j);
        std::memcpy(b.bufs[j].value().data(), v.data(), kValSize);
    }
    ASSERT_EQ(run(b, n), 0);
    for (int r : b.results) EXPECT_EQ(r, 0);
    EXPECT_EQ(read(store_, 7), val_of(7, n - 1));
}

// A put of a repeated key that fails does not stop the ones after it.
TEST_F(MultiPutTest, AFailedPutOfARepeatedKeyDoesNotStopTheLaterOnes) {
    Batch b = make_batch({5, 5, 5});
    b.bufs[1] = store_.alloc_put_buffer(kKeySize + 1, kValSize);  // -EINVAL
    std::string v = val_of(5, 99);
    std::memcpy(b.bufs[2].value().data(), v.data(), kValSize);
    ASSERT_EQ(run(b, kWindow), 0);
    EXPECT_EQ(b.results[0], 0);
    EXPECT_EQ(b.results[1], -EINVAL);
    EXPECT_EQ(b.results[2], 0);
    EXPECT_EQ(read(store_, 5), val_of(5, 99));
}

// Zero copy: the puts write from the PutBuffers.
TEST_F(MultiPutTest, ZeroCopiesNoValue) {
    Batch b = make_batch(iota(30));
    const uint64_t before = Store::thread_value_copies();
    ASSERT_EQ(run(b, kWindow), 0);
    EXPECT_EQ(Store::thread_value_copies(), before);
}

// A batch races other threads' gets and puts on the same store. Each value
// ever stored under a key is one of its generations, whole: a get sees one
// of them, never a mix. Under TSAN this is the check of the batch's own
// state against the store's.
TEST_F(MultiPutTest, RacesGetsAndOtherBatchesOnOtherThreads) {
    constexpr int kKeys = 64;
    constexpr int kGens = 30;
    {
        Batch b = make_batch(iota(kKeys), 0);
        ASSERT_EQ(run(b, kWindow), 0);
    }
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0}, reads{0};
    auto valid = [&](int i, const std::string& v) {
        for (int g = 0; g <= kGens; ++g)
            if (v == val_of(i, g) || v == val_of(i, 1000 + g)) return true;
        return false;
    };
    std::vector<std::thread> readers;
    for (int t = 0; t < 2; ++t)
        readers.emplace_back([&, t] {
            for (int i = t; !stop.load(); i = (i + 2) % kKeys) {
                std::string v = read(store_, i);
                reads.fetch_add(1);
                if (!valid(i, v)) bad.fetch_add(1);
            }
        });
    // Another batch writer, on its own thread and its own AIO context,
    // with the same keys at other generations.
    std::thread other([&] {
        for (int g = 0; g < kGens && !stop.load(); ++g) {
            Batch b = make_batch(iota(kKeys), 1000 + g);
            if (udepot::multi_put(store_, kKeys, b.key_ptrs.data(),
                                  b.key_sizes.data(), b.buf_ptrs.data(),
                                  b.results.data(), kWindow) != 0)
                bad.fetch_add(1);
            for (int r : b.results) bad.fetch_add(r != 0);
        }
    });
    for (int g = 1; g <= kGens; ++g) {
        Batch b = make_batch(iota(kKeys), g);
        ASSERT_EQ(run(b, kWindow), 0);
        for (int r : b.results) EXPECT_EQ(r, 0);
    }
    other.join();
    stop = true;
    for (auto& t : readers) t.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_GT(reads.load(), 0);
    for (int i = 0; i < kKeys; ++i) EXPECT_TRUE(valid(i, read(store_, i))) << i;
}
