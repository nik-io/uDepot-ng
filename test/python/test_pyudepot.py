#!/usr/bin/env python3
# Copyright (c) 2024-2026 Nikolas Ioannou
# SPDX-License-Identifier: BSD-3-Clause

import os
import re
import tempfile

import numpy as np
import pytest

import pyudepot


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


class TestMaxValue:
    """A record (14-byte header, key, value, 2-byte CRC, rounded up to
    4096-byte grains) must fit one segment's data grains (the segment less
    its one metadata grain) and a hash entry's 11-bit size (2047 grains):
    the longest value is the smaller of the two, and one byte more is
    refused.

    put() returns a bool, so these tests cannot tell the -EINVAL the store
    returns for a record that cannot fit from -ENOSPC or -ENOMEM; the C++
    tests (test/store_geometry_test.cc) check the code.
    """

    @pytest.mark.parametrize("table_size,size,seg_grains,grains,limit", [
        # Segment 514 grains: 513 of data, the segment is the limit.
        (2 * 1024 * 1024, 64 * 1024 * 1024, 514, 513, "the segment"),
        # Segment 2050 grains: the entry's 2047 grains are the limit.
        (8 * 1024 * 1024, TestTableSize._fitting_size(8 * 1024 * 1024),
         2050, 2047, "a hash entry"),
    ])
    @pytest.mark.parametrize("key_size", [1, 8, 100])
    def test_boundary(self, tmp_path, capfd, table_size, size, seg_grains,
                      grains, limit, key_size):
        kv = pyudepot.uDepot(file_name=str(tmp_path / "max"), size=size,
                             force_destroy=True, table_size=table_size)
        err = capfd.readouterr().err
        assert kv.grain_size() == 4096
        # The segment is what the comments above say, and one grain of it
        # is metadata: the 2047-grain term binds only past 2048 grains.
        assert kv.segment_size() // kv.grain_size() == seg_grains
        assert (seg_grains - 1 > 2047) == (grains == 2047)
        # The store's own message agrees with the accessors.
        m = re.search(r"udepot: (\d+) segments of (\d+) KiB \((\d+)-byte "
                      r"grains, (\d+) of them data\); the largest record is "
                      r"(\d+) grains, (\d+) bytes \(a value of up to (\d+) "
                      r"bytes under an 8-byte key\); .*Limited by ([a-z ]+)",
                      err)
        assert m, err
        assert kv.segment_size() == int(m.group(2)) * 1024
        assert int(m.group(3)) == kv.grain_size()
        assert int(m.group(5)) == grains
        assert int(m.group(6)) == grains * 4096
        assert int(m.group(7)) == kv.max_value_bytes(8)
        assert m.group(8).startswith(limit)

        n = kv.max_value_bytes(key_size)
        assert n == grains * 4096 - (14 + key_size + 2)
        key = np.full(key_size, 7, dtype=np.uint8)
        val = np.arange(n, dtype=np.uint64).astype(np.uint8)
        assert kv.put(key, val)
        out = np.zeros(n, dtype=np.uint8)
        assert kv.get(key, out)
        assert np.array_equal(out, val)

        key2 = np.full(key_size, 9, dtype=np.uint8)
        assert not kv.put(key2, np.zeros(n + 1, dtype=np.uint8))
        assert kv.exists(key2) is None
        kv._cleanup()

    def test_invalid_key_sizes_hold_no_value(self, store):
        assert store.max_value_bytes(0) == 0
        assert store.max_value_bytes(65536) == 0
        assert store.max_value_bytes(1) > 0
