#!/usr/bin/env python3
# Copyright (c) 2024-2026 Nikolas Ioannou
# SPDX-License-Identifier: BSD-3-Clause

import os
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

    @pytest.mark.parametrize("table_size", [4096, 1024 * 1024, 0])
    def test_configured_table_size_round_trips(self, tmp_path, table_size):
        kv = self._open(tmp_path, table_size=table_size)
        assert kv.put(_key("k"), _val("v"))
        out = np.zeros(1, dtype=np.uint8)
        assert kv.get(_key("k"), out)
        assert out.tobytes() == b"v"
        kv._cleanup()

    @pytest.mark.parametrize("table_size", [3000, 2048, 6 * 1024 * 1024])
    def test_invalid_table_size_fails_open(self, tmp_path, table_size):
        with pytest.raises(IOError):
            self._open(tmp_path, table_size=table_size)
