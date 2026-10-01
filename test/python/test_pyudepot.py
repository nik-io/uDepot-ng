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
    """Regression: closing freed the store under other callers.

    _cleanup() set the handle to None, so a later call passed NULL to the
    C library (a crash), and the C close deleted the store while another
    thread could still be inside a call (ctypes releases the GIL). Close
    now leaves the store allocated and calls fail cleanly.
    """

    def test_calls_after_close_fail_cleanly(self, tmp_path):
        kv = pyudepot.uDepot(file_name=str(tmp_path / "closed"),
                             size=STORE_SIZE, force_destroy=True)
        assert kv.put(_key("k"), _val("v"))
        kv._cleanup()
        assert not kv.put(_key("k"), _val("v"))
        assert not kv.get(_key("k"), np.zeros(8, dtype=np.uint8))
        assert not kv.delete(_key("k"))
        assert kv.exists(_key("k")) is None

    def test_close_while_threads_use_store(self, tmp_path):
        import threading

        kv = pyudepot.uDepot(file_name=str(tmp_path / "racing"),
                             size=STORE_SIZE, force_destroy=True)
        started = threading.Barrier(5)
        stop = threading.Event()

        def worker(tid):
            out = np.zeros(16, dtype=np.uint8)
            started.wait()
            i = 0
            while not stop.is_set():
                key = _key(f"t{tid}_{i % 32}")
                if i % 2:
                    kv.put(key, key)
                else:
                    kv.get(key, out)
                i += 1

        threads = [threading.Thread(target=worker, args=(t,))
                   for t in range(4)]
        for t in threads:
            t.start()
        started.wait()
        kv._cleanup()
        stop.set()
        for t in threads:
            t.join()
        assert not kv.put(_key("after"), _val("close"))
