#!/usr/bin/env python3
# Copyright (c) 2024-2026 Nikolas Ioannou
# SPDX-License-Identifier: BSD-3-Clause

import gc
import os
import sys
import tempfile

import numpy as np
import pytest

import pyudepot

# The module (the package's `uDepot` attribute is the class).
binding = sys.modules["pyudepot.uDepot"]


STORE_SIZE = 4 * 1024 * 1024


@pytest.fixture
def store(tmp_path):
    path = str(tmp_path / "pyudepot_test")
    kv = pyudepot.uDepot(file_name=path, size=STORE_SIZE, force_destroy=True)
    yield kv
    kv._cleanup()
    if os.path.exists(path):
        os.unlink(path)


def _key(s):
    return np.frombuffer(s.encode(), dtype=np.uint8).copy()


def _val(s):
    return np.frombuffer(s.encode(), dtype=np.uint8).copy()


class TestPutGet:
    def test_put_then_get_returns_value(self, store):
        key = _key("hello")
        val = _val("world")
        assert store.put(key, val)
        out = np.zeros(len(val), dtype=np.uint8)
        assert store.get(key, out)
        np.testing.assert_array_equal(out, val)

    def test_get_nonexistent_returns_false(self, store):
        key = _key("nosuchkey")
        out = np.zeros(64, dtype=np.uint8)
        assert not store.get(key, out)

    def test_put_multiple_keys(self, store):
        for i in range(50):
            key = _key(f"key_{i}")
            val = _val(f"val_{i}")
            assert store.put(key, val)

        for i in range(50):
            key = _key(f"key_{i}")
            expected = _val(f"val_{i}")
            out = np.zeros(len(expected), dtype=np.uint8)
            assert store.get(key, out)
            np.testing.assert_array_equal(out, expected)


class TestDelete:
    def test_delete_removes_key(self, store):
        key = _key("to_delete")
        val = _val("deleteme")
        assert store.put(key, val)
        assert store.delete(key)
        out = np.zeros(64, dtype=np.uint8)
        assert not store.get(key, out)

    def test_delete_nonexistent_returns_false(self, store):
        key = _key("missing_key")
        assert not store.delete(key)


class TestExists:
    def test_exists_returns_size(self, store):
        key = _key("present")
        val = _val("12345")
        assert store.put(key, val)
        size = store.exists(key)
        assert size == 5

    def test_exists_missing_returns_none(self, store):
        key = _key("absent")
        assert store.exists(key) is None


class TestLargeValue:
    def test_large_value_round_trips(self, store):
        key = _key("bigkey")
        val = np.full(4096, 0xAB, dtype=np.uint8)
        assert store.put(key, val)
        out = np.zeros(4096, dtype=np.uint8)
        assert store.get(key, out)
        np.testing.assert_array_equal(out, val)


class TestBinaryKeys:
    def test_binary_key_value(self, store):
        key = np.array([0xFF, 0x00, 0xDE, 0xAD], dtype=np.uint8)
        val = np.array([0xBE, 0xEF, 0xCA, 0xFE], dtype=np.uint8)
        assert store.put(key, val)
        out = np.zeros(4, dtype=np.uint8)
        assert store.get(key, out)
        np.testing.assert_array_equal(out, val)


class TestClose:
    """After close the handle is gone: calls raise instead of passing a
    freed pointer to C. As in uDepot, no call may race the close."""

    def test_calls_after_close_raise(self, tmp_path):
        kv = pyudepot.uDepot(file_name=str(tmp_path / "closed"),
                             size=STORE_SIZE, force_destroy=True)
        assert kv.put(_key("k"), _val("v"))
        kv._cleanup()
        with pytest.raises(ValueError):
            kv.put(_key("k"), _val("v"))
        with pytest.raises(ValueError):
            kv.get(_key("k"), np.zeros(8, dtype=np.uint8))
        with pytest.raises(ValueError):
            kv.delete(_key("k"))
        with pytest.raises(ValueError):
            kv.exists(_key("k"))
        kv._cleanup()  # closing twice is harmless


class TestForceDestroy:
    """force_destroy reaches the store: a reopen under another size finds no
    store and fails unless asked to start a new one; under the size it was
    created with, it restores the store."""

    def test_force_destroy_is_honoured(self, tmp_path):
        path = str(tmp_path / "force")
        other = STORE_SIZE + 1024 * 1024
        kv = pyudepot.uDepot(file_name=path, size=STORE_SIZE,
                             force_destroy=True)
        assert kv.put(_key("keep"), _val("me"))
        kv._cleanup()

        # Another size, no force_destroy: refused.
        with pytest.raises(IOError):
            pyudepot.uDepot(file_name=path, size=other, force_destroy=False)

        # The size it was created with: the key is there.
        kv = pyudepot.uDepot(file_name=path, size=STORE_SIZE,
                             force_destroy=False)
        out = np.zeros(2, dtype=np.uint8)
        assert kv.get(_key("keep"), out)
        np.testing.assert_array_equal(out, _val("me"))
        kv._cleanup()

        # Another size, force_destroy: a new store, the key is gone.
        kv = pyudepot.uDepot(file_name=path, size=other, force_destroy=True)
        assert not kv.get(_key("keep"), np.zeros(2, dtype=np.uint8))
        kv._cleanup()


class TestTableSize:
    """The index table size is the caller's: 2 MiB by default, for tests;
    any power of two of at least 4096 bytes, or 0 for the store's default.
    A table too small for huge pages makes the store say so at open."""

    def _open(self, tmp_path, **kwargs):
        return pyudepot.uDepot(file_name=str(tmp_path / "tables"),
                               size=STORE_SIZE, force_destroy=True, **kwargs)

    def test_default_is_2_mib_and_says_so(self, tmp_path, capfd):
        assert pyudepot.DEFAULT_TABLE_SIZE == 2 * 1024 * 1024
        # Room for 32 segments of a 2 MiB table (2 MiB + 8 KiB each), and
        # the device metadata's tail; a smaller device halves the segment.
        kv = pyudepot.uDepot(file_name=str(tmp_path / "tables"),
                             size=32 * (2 * 1024 * 1024 + 8192) + 4096,
                             force_destroy=True)
        assert kv.put(_key("k"), _val("v"))
        kv._cleanup()
        err = capfd.readouterr().err
        assert "index tables map on 4 KiB pages" in err
        assert "directory grows to be slower" in err
        assert "do not fit" not in err  # the device has room for them

    def test_a_device_too_small_for_the_tables_says_so(self, tmp_path, capfd):
        # 4 MiB holds too few 2 MiB-table segments: the store makes them
        # smaller, and with them the tables, and says so.
        kv = self._open(tmp_path)
        assert kv.put(_key("k"), _val("v"))
        kv._cleanup()
        err = capfd.readouterr().err
        assert "the requested segments of" in err
        assert "do not fit" in err

    @pytest.mark.parametrize("table_size,named", [
        (1024 * 1024, "the requested segments of 1032 KiB (index tables of 1024 KiB)"),
        (2 * 1024 * 1024, "the requested segments of 2056 KiB (index tables of 2048 KiB)"),
        (0, "the default segments of"),
    ])
    def test_the_table_size_reaches_the_store(self, tmp_path, capfd,
                                              table_size, named):
        # On a device too small for them the store names the tables it was
        # asked for: the size given here, not the binding's default.
        kv = self._open(tmp_path, table_size=table_size)
        kv._cleanup()
        assert named in capfd.readouterr().err

    @staticmethod
    def _fitting_size(table_size):
        # Room for 32 segments of the table and the device metadata's tail.
        page = 4096
        seg = (table_size + 2 * 512 + 32 * 8 + page + page - 1) // page * page
        return 32 * seg + 4096

    @pytest.mark.parametrize("table_size", [4096, 1024 * 1024])
    def test_configured_table_size_round_trips(self, tmp_path, capfd,
                                               table_size):
        kv = pyudepot.uDepot(file_name=str(tmp_path / "tables"),
                             size=self._fitting_size(table_size),
                             force_destroy=True, table_size=table_size)
        assert kv.put(_key("k"), _val("v"))
        out = np.zeros(1, dtype=np.uint8)
        assert kv.get(_key("k"), out)
        assert out.tobytes() == b"v"
        kv._cleanup()
        assert "do not fit" not in capfd.readouterr().err

    def test_the_default_table_size_round_trips(self, tmp_path):
        kv = self._open(tmp_path, table_size=0)
        assert kv.put(_key("k"), _val("v"))
        out = np.zeros(1, dtype=np.uint8)
        assert kv.get(_key("k"), out)
        assert out.tobytes() == b"v"
        kv._cleanup()

    @pytest.mark.parametrize("table_size",
                             [3000, 2048, 6 * 1024 * 1024, 1 << 41, 1 << 63])
    def test_invalid_table_size_fails_open(self, tmp_path, table_size):
        with pytest.raises(IOError):
            self._open(tmp_path, table_size=table_size)


# ---------------------------------------------------------------------------
# Zero copy: PutBuffer / GetBuffer.

GRAIN = 4096
MAX_RECORD = 2047 * GRAIN          # the largest record: 11 bits of grains
KV_HEADER = 14                     # sizeof(KvHeader), packed
KV_SUFFIX = 2                      # the record's crc16


def _max_value(key_size):
    return MAX_RECORD - KV_HEADER - key_size - KV_SUFFIX


def _addr(arr):
    return arr.__array_interface__["data"][0]


def _bytes_of(n, seed):
    return np.random.default_rng(seed).integers(0, 256, n, dtype=np.uint8)


def _zc_put(kv, key, data):
    pb = kv.alloc_put_buffer(key.size, data.size)
    pb.value[:] = data
    assert kv.put_buffer(key, pb)


def _zc_get(kv, key, size):
    gb = kv.alloc_get_buffer(key.size, size)
    assert kv.get_into(key, gb)
    return gb.value


@pytest.fixture
def big_store(tmp_path):
    # A record must fit a segment: the 4 MiB store above shrinks its
    # segments to 92 KiB and refuses anything larger, so the largest record
    # (8 MiB) needs tables of 16 MiB and a device that holds several of them.
    path = str(tmp_path / "pyudepot_big")
    kv = pyudepot.uDepot(file_name=path, size=256 * 1024 * 1024,
                         force_destroy=True, table_size=16 * 1024 * 1024)
    yield kv
    kv._cleanup()


@pytest.fixture
def frees(monkeypatch):
    """Counts the C buffers freed: [put, get]."""
    gc.collect()  # other tests' garbage is not this test's frees
    n = [0, 0]
    real_put, real_get = binding.pyfreeput, binding.pyfreeget

    def put(h):
        n[0] += 1
        real_put(h)

    def get(h):
        n[1] += 1
        real_get(h)

    monkeypatch.setattr(binding, "pyfreeput", put)
    monkeypatch.setattr(binding, "pyfreeget", get)
    return n


class TestZeroCopyRoundTrip:
    @pytest.mark.parametrize("size", [0, 1, 100, 4000, 4096, 100_000,
                                      256 * 1024])
    def test_put_buffer_then_get_into(self, big_store, size):
        store = big_store
        key, data = _key("zc-key"), _bytes_of(size, size)
        _zc_put(store, key, data)
        np.testing.assert_array_equal(_zc_get(store, key, size), data)
        # And the copying interface sees the same bytes.
        out = np.zeros(size, dtype=np.uint8)
        assert store.get(key, out)
        np.testing.assert_array_equal(out, data)
        assert store.exists(key) == size

    def test_copying_put_then_get_into(self, store):
        key, data = _key("copy-put"), _bytes_of(5000, 1)
        assert store.put(key, data)
        np.testing.assert_array_equal(_zc_get(store, key, 5000), data)

    def test_a_value_near_the_largest_the_store_holds(self, big_store):
        key = _key("big-key")
        data = _bytes_of(_max_value(key.size), 7)
        _zc_put(big_store, key, data)
        np.testing.assert_array_equal(
            _zc_get(big_store, key, data.size), data)

    def test_many_keys_reusing_one_buffer_each(self, store):
        pb = store.alloc_put_buffer(8, 3000)
        gb = store.alloc_get_buffer(8, 3000)
        for i in range(200):
            key = _key("key%05d" % i)
            data = _bytes_of(3000, i)
            pb.value[:] = data
            assert store.put_buffer(key, pb)
        for i in range(200):
            assert store.get_into(_key("key%05d" % i), gb)
            np.testing.assert_array_equal(gb.value, _bytes_of(3000, i))

    def test_get_into_missing_key(self, store):
        gb = store.alloc_get_buffer(8, 100)
        assert not store.get_into(_key("missing!"), gb)
        assert gb.value.size == 0

    def test_a_failed_get_empties_value_and_keeps_the_buffer(self, store):
        key = _key("present!")
        _zc_put(store, key, _bytes_of(100, 3))
        gb = store.alloc_get_buffer(8, 100)
        assert store.get_into(key, gb)
        assert gb.value.size == 100
        assert not store.get_into(_key("missing!"), gb)
        assert gb.value.size == 0
        assert store.get_into(key, gb)
        np.testing.assert_array_equal(gb.value, _bytes_of(100, 3))

    def test_get_value_is_read_only_and_put_value_writable(self, store):
        key = _key("flags!!!")
        pb = store.alloc_put_buffer(8, 10)
        assert pb.value.flags.writeable
        assert store.put_buffer(key, pb)
        gb = store.alloc_get_buffer(8, 10)
        assert store.get_into(key, gb)
        assert not gb.value.flags.writeable
        assert gb.value.dtype == np.uint8


class TestZeroCopyIsZeroCopy:
    def test_zero_copy_put_and_get_copy_no_value(self, big_store):
        store = big_store
        key, data = _key("nocopy!!"), _bytes_of(256 * 1024, 5)
        pb = store.alloc_put_buffer(key.size, data.size)
        gb = store.alloc_get_buffer(key.size, data.size)
        pb.value[:] = data  # the caller's own write, not uDepot's
        before = pyudepot.thread_value_copies()
        assert store.put_buffer(key, pb)
        assert store.get_into(key, gb)
        assert pyudepot.thread_value_copies() == before
        np.testing.assert_array_equal(gb.value, data)

    def test_copying_put_and_get_each_copy_the_value(self, big_store):
        store = big_store
        # The control: the counter does move when a copy happens.
        key, data = _key("control!"), _bytes_of(256 * 1024, 5)
        out = np.zeros(data.size, dtype=np.uint8)
        before = pyudepot.thread_value_copies()
        assert store.put(key, data)
        assert pyudepot.thread_value_copies() == before + 1
        assert store.get(key, out)
        assert pyudepot.thread_value_copies() == before + 2

    def test_the_value_lives_in_the_buffer_and_it_is_reused(self, store):
        # The value is read in place: it sits (14 + key_size) bytes into a
        # 4096-aligned record buffer, and a get that fits reads into the same
        # buffer again, so the address does not move.
        k1, k2 = _key("addr-one"), _key("addr-two")
        _zc_put(store, k1, _bytes_of(3000, 1))
        _zc_put(store, k2, _bytes_of(1000, 2))
        gb = store.alloc_get_buffer(8, 3000)
        assert store.get_into(k1, gb)
        first = _addr(gb.value)
        assert first % 4096 == (KV_HEADER + 8) % 4096
        assert store.get_into(k2, gb)
        assert _addr(gb.value) == first

    def test_a_put_value_starts_at_the_documented_offset(self, store):
        for key_size in (1, 8, 100, 4081):
            pb = store.alloc_put_buffer(key_size, 10)
            assert _addr(pb.value) % 4096 == (KV_HEADER + key_size) % 4096


class TestGetBufferReplacement:
    def test_a_buffer_too_small_for_the_value_is_replaced(self, big_store):
        store = big_store
        small, big = _key("small!!!"), _key("big!!!!!")
        _zc_put(store, small, _bytes_of(10, 1))
        _zc_put(store, big, _bytes_of(100_000, 2))
        gb = store.alloc_get_buffer(8, 10)
        assert store.get_into(small, gb)
        first = _addr(gb.value)
        assert store.get_into(big, gb)
        np.testing.assert_array_equal(gb.value, _bytes_of(100_000, 2))
        assert _addr(gb.value) != first  # a new buffer
        # The new one is the buffer now: a smaller value reads into it.
        second = _addr(gb.value)
        assert store.get_into(small, gb)
        assert _addr(gb.value) == second
        np.testing.assert_array_equal(gb.value, _bytes_of(10, 1))

    def test_an_earlier_array_keeps_its_bytes_through_a_get(self, store):
        a, b = _key("first!!!"), _key("second!!")
        _zc_put(store, a, _bytes_of(3000, 1))
        _zc_put(store, b, _bytes_of(3000, 2))
        gb = store.alloc_get_buffer(8, 3000)
        assert store.get_into(a, gb)
        v1 = gb.value
        assert store.get_into(b, gb)  # fits: would overwrite v1 in place
        v2 = gb.value
        np.testing.assert_array_equal(v1, _bytes_of(3000, 1))
        np.testing.assert_array_equal(v2, _bytes_of(3000, 2))
        assert _addr(v1) != _addr(v2)

    def test_an_earlier_array_survives_a_replacing_get(self, big_store,
                                                       frees):
        store = big_store
        small, big = _key("small!!!"), _key("big!!!!!")
        _zc_put(store, small, _bytes_of(10, 1))
        _zc_put(store, big, _bytes_of(100_000, 2))
        gb = store.alloc_get_buffer(8, 10)
        assert store.get_into(small, gb)
        v1 = gb.value
        assert store.get_into(big, gb)  # too small: would free v1's memory
        # Reuse the freed-chunk size class, so a freed v1 would be clobbered.
        junk = [store.alloc_put_buffer(8, 10) for _ in range(8)]
        for j in junk:
            j.value[:] = 0xEE
        np.testing.assert_array_equal(v1, _bytes_of(10, 1))
        assert frees[1] == 0  # v1's buffer is not freed while v1 lives
        del v1
        gc.collect()
        assert frees[1] == 1

    def test_a_failed_get_leaves_an_earlier_array_alone(self, store):
        key = _key("present!")
        _zc_put(store, key, _bytes_of(100, 4))
        gb = store.alloc_get_buffer(8, 100)
        assert store.get_into(key, gb)
        v1 = gb.value
        assert not store.get_into(_key("missing!"), gb)
        np.testing.assert_array_equal(v1, _bytes_of(100, 4))
        assert gb.value.size == 0
        assert store.get_into(key, gb)
        np.testing.assert_array_equal(gb.value, _bytes_of(100, 4))

    def test_without_live_arrays_the_buffer_is_reused(self, store, frees):
        key = _key("reuse!!!")
        _zc_put(store, key, _bytes_of(100, 4))
        gb = store.alloc_get_buffer(8, 100)
        addrs = set()
        for _ in range(5):
            assert store.get_into(key, gb)
            addrs.add(_addr(gb.value))  # the array is dropped right away
        assert len(addrs) == 1
        assert frees[1] == 0


class TestBufferLifetime:
    def test_freed_once_when_dropped(self, store, frees):
        pb = store.alloc_put_buffer(8, 100)
        gb = store.alloc_get_buffer(8, 100)
        pb.value[:] = 1
        assert frees == [0, 0]
        del pb, gb
        gc.collect()
        assert frees == [1, 1]
        gc.collect()
        assert frees == [1, 1]

    def test_views_keep_the_buffer_alive(self, store, frees):
        pb = store.alloc_put_buffer(8, 1000)
        direct = pb.value
        views = [direct[10:20], direct.reshape(10, 100),
                 np.frombuffer(memoryview(direct), dtype=np.uint8)[5:],
                 direct.view(np.uint8)[::2]]
        direct[:] = 42
        del pb, direct
        gc.collect()
        assert frees[0] == 0
        assert all((v == 42).all() for v in views)  # still readable,
        for i in range(len(views)):                 # and writable memory
            views[i][...] = 7
        while views:
            assert frees[0] == 0
            views.pop()
            gc.collect()
        assert frees[0] == 1

    def test_get_views_keep_the_buffer_alive(self, store, frees):
        key = _key("lifetime")
        _zc_put(store, key, _bytes_of(1000, 9))
        gb = store.alloc_get_buffer(8, 1000)
        assert store.get_into(key, gb)
        view = gb.value[100:200]
        del gb
        gc.collect()
        assert frees[1] == 0
        np.testing.assert_array_equal(view, _bytes_of(1000, 9)[100:200])
        del view
        gc.collect()
        assert frees[1] == 1

    def test_a_buffer_outlives_a_closed_store(self, tmp_path, frees):
        kv = pyudepot.uDepot(file_name=str(tmp_path / "closing"),
                             size=STORE_SIZE, force_destroy=True)
        pb = kv.alloc_put_buffer(8, 100)
        view = pb.value
        kv._cleanup()
        view[:] = 3  # the memory is the buffer's, not the store's
        with pytest.raises(ValueError, match="closed"):
            kv.put_buffer(_key("12345678"), pb)
        with pytest.raises(ValueError, match="closed"):
            kv.alloc_get_buffer(8, 100)
        del pb, view
        gc.collect()
        assert frees[0] == 1

    def test_ten_thousand_cycles_do_not_leak(self, store):
        def rss_mib():
            with open("/proc/self/statm") as f:
                return int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE") / 2**20

        key = _key("cycle!!!")
        _zc_put(store, key, _bytes_of(64 * 1024, 1))
        for _ in range(200):  # warm up the allocator
            store.alloc_put_buffer(8, 64 * 1024).value[:] = 1
        before = rss_mib()
        for i in range(10_000):
            pb = store.alloc_put_buffer(8, 64 * 1024)
            pb.value[:] = i & 0xFF
            gb = store.alloc_get_buffer(8, 64 * 1024)
            if i % 100 == 0:
                assert store.get_into(key, gb)
                assert gb.value.size == 64 * 1024
        # A leak would be 10k * 2 * 68 KiB = 1.3 GiB.
        assert rss_mib() - before < 50


class TestZeroCopyErrors:
    def test_wrong_key_size_raises(self, store):
        pb = store.alloc_put_buffer(8, 10)
        with pytest.raises(ValueError, match="allocated for 8"):
            store.put_buffer(_key("short"), pb)
        with pytest.raises(ValueError):
            store.put_buffer(_key("much too long"), pb)
        assert store.exists(_key("short")) is None

    def test_the_store_refuses_a_wrong_key_size_too(self, store):
        # The C ABI, for a caller that skips the Python check.
        pb = store.alloc_put_buffer(8, 10)
        key = _key("short")
        rc = binding.pyputbuf(store._kv, key, key.size, pb._owned.handle)
        assert rc == -22  # -EINVAL

    @pytest.mark.parametrize("key_size, val_size", [
        (8, _max_value(8) + 1), (0, 10), (65536, 10), (8, 2**32 - 1),
        (8, 2**40), (-1, 10), (8, -1),
        (2**32 + 8, 10), (8, 2**64 + 5)])  # ctypes would wrap these to 8, 5
    def test_unstorable_sizes_raise_with_the_sizes(self, store, key_size,
                                                   val_size):
        for alloc in (store.alloc_put_buffer, store.alloc_get_buffer):
            with pytest.raises(ValueError) as e:
                alloc(key_size, val_size)
            assert "key_size={} val_size={}".format(
                key_size, val_size) in str(e.value)

    def test_a_too_large_record_names_the_limit(self, store):
        with pytest.raises(ValueError, match="2048 grains.*at most 2047"):
            store.alloc_put_buffer(8, _max_value(8) + 1)

    def test_the_c_abi_says_so_on_stderr_when_it_refuses(self, store, capfd):
        h = binding.pyallocput(store._kv, 8, _max_value(8) + 1)
        assert not h
        assert "cannot allocate a put buffer for a key of 8 bytes" in \
            capfd.readouterr().err
        h = binding.pyallocget(store._kv, 8, _max_value(8) + 1)
        assert not h
        assert "cannot allocate a get buffer for a key of 8 bytes" in \
            capfd.readouterr().err

    def test_a_failed_allocation_is_a_memory_error(self, store, monkeypatch):
        monkeypatch.setattr(binding, "pyallocput", lambda *a: None)
        with pytest.raises(MemoryError, match="key_size=8 val_size=10"):
            store.alloc_put_buffer(8, 10)

    def test_a_failed_new_get_buffer_is_a_memory_error(self, store,
                                                       monkeypatch):
        key = _key("present!")
        _zc_put(store, key, _bytes_of(10, 1))
        gb = store.alloc_get_buffer(8, 10)
        assert store.get_into(key, gb)
        held = gb.value  # forces a new buffer for the next get
        monkeypatch.setattr(binding, "pynewget", lambda: None)
        with pytest.raises(MemoryError, match="cannot allocate a get buffer"):
            store.get_into(key, gb)
        assert held.size == 10

    def test_the_largest_record_allocates(self, store):
        store.alloc_put_buffer(8, _max_value(8))
        store.alloc_get_buffer(8, _max_value(8))

    def test_a_buffer_of_another_store_is_refused(self, store, tmp_path):
        other = pyudepot.uDepot(file_name=str(tmp_path / "other"),
                                size=STORE_SIZE, force_destroy=True)
        try:
            pb = other.alloc_put_buffer(8, 10)
            gb = other.alloc_get_buffer(8, 10)
            with pytest.raises(ValueError, match="PutBuffer of this store"):
                store.put_buffer(_key("12345678"), pb)
            with pytest.raises(ValueError, match="GetBuffer of this store"):
                store.get_into(_key("12345678"), gb)
        finally:
            other._cleanup()

    def test_things_that_are_not_buffers_are_refused(self, store):
        gb = store.alloc_get_buffer(8, 10)
        pb = store.alloc_put_buffer(8, 10)
        with pytest.raises(ValueError):
            store.put_buffer(_key("12345678"), np.zeros(10, dtype=np.uint8))
        with pytest.raises(ValueError):
            store.put_buffer(_key("12345678"), gb)
        with pytest.raises(ValueError):
            store.get_into(_key("12345678"), pb)
        with pytest.raises(ValueError):
            store.get_into(_key("12345678"), None)


class TestZeroCopyReachesTheCallersBuffer:
    def test_put_buffer_writes_the_key_into_the_callers_buffer(self, store):
        # put_record writes header, key and checksum around the value in the
        # caller's own PutBuffer (store.cc: the key is copied to
        # p + sizeof(KvHeader), i.e. right before value()), so the bytes just
        # before pb.value are the key only if the buffer itself was put, and
        # not a copy of it.
        import ctypes
        key = _key("in-place-key")
        pb = store.alloc_put_buffer(key.size, 100)
        pb.value[:] = 5
        addr = _addr(pb.value)
        before = bytes((ctypes.c_ubyte * key.size).from_address(
            addr - key.size))
        assert before != key.tobytes()
        assert store.put_buffer(key, pb)
        after = bytes((ctypes.c_ubyte * key.size).from_address(
            addr - key.size))
        assert after == key.tobytes()

