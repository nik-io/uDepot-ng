#!/usr/bin/env python3
# Copyright (c) 2024-2026 Nikolas Ioannou
# SPDX-License-Identifier: BSD-3-Clause

import errno
import gc
import os
import re
import subprocess
import sys
import tempfile
import weakref

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
    kv.close()
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
    """close() releases the store now. After it the handle is gone: calls
    raise instead of passing a freed pointer to C. As in uDepot, no call may
    race the close."""

    def _open(self, tmp_path, **kwargs):
        kwargs.setdefault("force_destroy", True)
        return pyudepot.uDepot(file_name=str(tmp_path / "closed"),
                               size=STORE_SIZE, **kwargs)

    @pytest.fixture
    def closes(self, monkeypatch):
        """The handles passed to the C close. One that is not open (a second
        close of the same handle) is recorded and not passed on, so that a
        double close fails the test instead of double-freeing."""
        calls, live = [], set()
        real_open, real_close = binding.pyopen, binding.pyclose

        def open_(*args):
            handle = real_open(*args)
            live.add(handle)
            return handle

        def close(handle):
            calls.append(handle)
            if handle in live:
                live.remove(handle)
                real_close(handle)

        monkeypatch.setattr(binding, "pyopen", open_)
        monkeypatch.setattr(binding, "pyclose", close)
        return calls

    def test_calls_after_close_raise(self, tmp_path):
        kv = self._open(tmp_path)
        assert kv.put(_key("k"), _val("v"))
        kv.close()
        with pytest.raises(ValueError):
            kv.put(_key("k"), _val("v"))
        with pytest.raises(ValueError):
            kv.get(_key("k"), np.zeros(8, dtype=np.uint8))
        with pytest.raises(ValueError):
            kv.delete(_key("k"))
        with pytest.raises(ValueError):
            kv.exists(_key("k"))
        kv.close()  # closing twice is harmless

    def test_every_method_raises_closed_after_close(self, tmp_path):
        kv = self._open(tmp_path)
        key, val = _key("12345678"), _val("v")
        pb = kv.alloc_put_buffer(key.size, 100)
        gb = kv.alloc_get_buffer(key.size, 100)
        assert kv.put(key, val)
        kv.close()
        out = np.zeros(8, dtype=np.uint8)
        calls = {
            "get": lambda: kv.get(key, out),
            "put": lambda: kv.put(key, val),
            "delete": lambda: kv.delete(key),
            "exists": lambda: kv.exists(key),
            "alloc_put_buffer": lambda: kv.alloc_put_buffer(8, 100),
            "alloc_get_buffer": lambda: kv.alloc_get_buffer(8, 100),
            "put_buffer": lambda: kv.put_buffer(key, pb),
            "get_into": lambda: kv.get_into(key, gb),
            "multi_get_into": lambda: kv.multi_get_into([key], [gb]),
            "multi_get_into (none)": lambda: kv.multi_get_into([], []),
        }
        for name, call in calls.items():
            with pytest.raises(ValueError,
                               match="uDepot store is closed") as e:
                call()
            assert e.type is ValueError, name

    def test_close_releases_the_store_once(self, tmp_path, closes):
        kv = self._open(tmp_path)
        handle = kv._kv
        kv.close()
        kv.close()
        assert kv._kv is None
        assert closes == [handle]  # once: the second close called nothing

    def test_close_is_a_context_manager(self, tmp_path, closes):
        with self._open(tmp_path) as kv:
            assert kv.put(_key("k"), _val("v"))
            assert closes == []
        assert len(closes) == 1
        with pytest.raises(ValueError, match="closed"):
            kv.get(_key("k"), np.zeros(1, dtype=np.uint8))

    def test_the_context_manager_closes_on_an_exception(self, tmp_path,
                                                         closes):
        with pytest.raises(KeyError):
            with self._open(tmp_path) as kv:
                raise KeyError("body")
        assert len(closes) == 1

    def test_a_closed_store_is_not_kept_alive_by_the_exit_hook(self,
                                                              tmp_path):
        # atexit.register(self.close) holds the store until exit; close()
        # takes the hook back, so a closed store can be collected.
        kv = self._open(tmp_path)
        ref = weakref.ref(kv)
        kv.close()
        del kv
        gc.collect()
        assert ref() is None

    def test_an_open_store_is_closed_at_exit(self, tmp_path):
        # The hook still closes a store nobody did: the process exits with
        # the C close called once for it.
        script = (
            "import atexit, sys\n"
            "import numpy as np\n"
            "import pyudepot\n"
            "b = sys.modules['pyudepot.uDepot']\n"
            "calls = []\n"
            "real = b.pyclose\n"
            "def close(h):\n"
            "    calls.append(h)\n"
            "    real(h)\n"
            "b.pyclose = close\n"
            "atexit.register(lambda: print('CLOSES', len(calls)))\n"
            "kv = pyudepot.uDepot(file_name=sys.argv[1], size=%d,"
            " force_destroy=True)\n"
            "gone = pyudepot.uDepot(file_name=sys.argv[1] + '2', size=%d,"
            " force_destroy=True)\n"
            "gone.close()\n"
            "print('EXPLICIT', len(calls))\n" % (STORE_SIZE, STORE_SIZE))
        # atexit runs last-registered first: the print above is registered
        # before the stores' hooks, so it runs after them.
        r = subprocess.run(
            [sys.executable, "-c", script, str(tmp_path / "exit")],
            capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, r.stderr
        # `gone` is closed by the call, `kv` by the exit hook.
        assert "EXPLICIT 1" in r.stdout, r.stdout + r.stderr
        assert "CLOSES 2" in r.stdout, r.stdout + r.stderr

    def test_data_survives_a_close_and_a_reopen_in_this_process(self,
                                                                tmp_path):
        keys = [_key("key%04d" % i) for i in range(50)]
        kv = self._open(tmp_path)
        for i, k in enumerate(keys[:25]):
            assert kv.put(k, _val("copy%d" % i))
        for i, k in enumerate(keys[25:]):
            _zc_put(kv, k, _bytes_of(1000 + i, i))
        assert kv.delete(keys[3])
        kv.close()
        kv = self._open(tmp_path, force_destroy=False)
        for i, k in enumerate(keys[:25]):
            out = np.zeros(16, dtype=np.uint8)
            if i == 3:
                assert not kv.get(k, out)
                continue
            assert kv.get(k, out)
            assert out[:len("copy%d" % i)].tobytes() == ("copy%d" % i).encode()
        for i, k in enumerate(keys[25:]):
            np.testing.assert_array_equal(_zc_get(kv, k, 1000 + i),
                                          _bytes_of(1000 + i, i))
        kv.close()

    def test_a_reopened_store_closes_independently(self, tmp_path, closes):
        first = self._open(tmp_path)
        first.close()
        second = self._open(tmp_path, force_destroy=False)
        first.close()  # the earlier object's close does not touch the new one
        assert second.exists(_key("nothing")) is None
        second.close()
        assert len(closes) == 2

    def test_buffers_stay_valid_after_close(self, tmp_path):
        kv = self._open(tmp_path)
        _zc_put(kv, _key("k"), _bytes_of(300, 5))
        gb = kv.alloc_get_buffer(1, 300)
        assert kv.get_into(_key("k"), gb)
        value = gb.value
        pb = kv.alloc_put_buffer(1, 100)
        written = pb.value
        kv.close()
        np.testing.assert_array_equal(value, _bytes_of(300, 5))
        written[:] = 7  # the memory is the buffer's, not the store's
        assert (written == 7).all()


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
        kv.close()

        # Another size, no force_destroy: refused.
        with pytest.raises(IOError):
            pyudepot.uDepot(file_name=path, size=other, force_destroy=False)

        # The size it was created with: the key is there.
        kv = pyudepot.uDepot(file_name=path, size=STORE_SIZE,
                             force_destroy=False)
        out = np.zeros(2, dtype=np.uint8)
        assert kv.get(_key("keep"), out)
        np.testing.assert_array_equal(out, _val("me"))
        kv.close()

        # Another size, force_destroy: a new store, the key is gone.
        kv = pyudepot.uDepot(file_name=path, size=other, force_destroy=True)
        assert not kv.get(_key("keep"), np.zeros(2, dtype=np.uint8))
        kv.close()


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
        kv.close()
        err = capfd.readouterr().err
        assert "index tables map on 4 KiB pages" in err
        assert "directory grows to be slower" in err
        assert "do not fit" not in err  # the device has room for them

    def test_a_device_too_small_for_the_tables_says_so(self, tmp_path, capfd):
        # 4 MiB holds too few 2 MiB-table segments: the store makes them
        # smaller, and with them the tables, and says so.
        kv = self._open(tmp_path)
        assert kv.put(_key("k"), _val("v"))
        kv.close()
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
        kv.close()
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
        kv.close()
        assert "do not fit" not in capfd.readouterr().err

    def test_the_default_table_size_round_trips(self, tmp_path):
        kv = self._open(tmp_path, table_size=0)
        assert kv.put(_key("k"), _val("v"))
        out = np.zeros(1, dtype=np.uint8)
        assert kv.get(_key("k"), out)
        assert out.tobytes() == b"v"
        kv.close()

    @pytest.mark.parametrize("table_size",
                             [3000, 2048, 6 * 1024 * 1024, 1 << 41, 1 << 63])
    def test_invalid_table_size_fails_open(self, tmp_path, table_size):
        with pytest.raises(IOError):
            self._open(tmp_path, table_size=table_size)


_GRAINS = [512, 4096, 32768]


class TestGrainSize:
    """grain_size is the caller's: a power of two from 512 to 1 MiB, 4096 by
    default. A larger grain stores larger values (a record is at most 2047
    grains); a store is reopened with the grain it was created with."""

    def _open(self, tmp_path, grain_size, **kwargs):
        kwargs.setdefault("size", STORE_SIZE)
        kwargs.setdefault("force_destroy", True)
        return pyudepot.uDepot(file_name=str(tmp_path / "grains"),
                               grain_size=grain_size, **kwargs)

    def test_default_is_4096(self, tmp_path):
        assert pyudepot.DEFAULT_GRAIN_SIZE == 4096
        kv = pyudepot.uDepot(file_name=str(tmp_path / "grains"),
                             size=STORE_SIZE, force_destroy=True)
        assert kv._grain_size == 4096
        kv.close()

    @pytest.mark.parametrize("grain", _GRAINS)
    def test_each_grain_round_trips(self, tmp_path, grain):
        kv = self._open(tmp_path, grain)
        assert kv.put(_key("k"), _val("value"))
        out = np.zeros(5, dtype=np.uint8)
        assert kv.get(_key("k"), out)
        assert out.tobytes() == b"value"
        pb = kv.alloc_put_buffer(1, 100)
        pb.value[:] = 9
        assert kv.put_buffer(_key("z"), pb)
        assert _zc_get(kv, _key("z"), 100).tobytes() == b"\x09" * 100
        kv.close()

    @pytest.mark.parametrize("grain", [0, 1, 256, 511, 513, 3000, 65535,
                                       2 * 1024 * 1024, 4 * 1024 * 1024])
    def test_an_invalid_grain_fails_the_open_naming_it(self, tmp_path, capfd,
                                                      grain):
        path = tmp_path / "grains"
        with pytest.raises((IOError, ValueError)) as e:
            self._open(tmp_path, grain)
        assert "grain_size={}".format(grain) in str(e.value) or \
            "grain_size={!r}".format(grain) in str(e.value)
        if grain != 0:  # 0 is refused before the C layer
            err = capfd.readouterr().err
            assert "grain size {} is not valid".format(grain) in err
            assert "a power of two from 512 to 1048576 bytes" in err
        assert not path.exists(), "a refused grain must not create the file"

    @pytest.mark.parametrize("grain", [2**32 + 4096, -4096, 4096.0, "4096",
                                       None])
    def test_a_grain_ctypes_would_wrap_or_coerce_is_refused(self, tmp_path,
                                                           grain):
        # 2**32 + 4096 would reach the C layer as a valid 4096.
        with pytest.raises(ValueError, match="grain_size="):
            self._open(tmp_path, grain)

    def test_the_largest_grain_opens_and_round_trips(self, tmp_path):
        # 1 MiB: 2047 of them are one write below Linux's 2147479552-byte cap.
        kv = self._open(tmp_path, 1 << 20, size=128 * 1024 * 1024 + 4096,
                        table_size=1 << 20)
        assert kv.put(_key("k"), _val("value"))
        out = np.zeros(5, dtype=np.uint8)
        assert kv.get(_key("k"), out)
        assert out.tobytes() == b"value"
        kv.close()

    def test_a_larger_grain_stores_a_larger_value(self, tmp_path, capfd):
        # A record is at most 2047 grains: ~8 MiB at 4096, ~64 MiB at 32768.
        # The record must also fit a segment, so the tables are 16 MiB and
        # the device holds 32 of their segments: the grain is the only
        # difference between the two stores (checked: the store did not
        # shrink its segments).
        table = 16 * 1024 * 1024
        size = TestTableSize._fitting_size(table)
        value = _bytes_of(2047 * 4096 + 5000, 3)
        key = _key("big")
        out = np.zeros(value.size, dtype=np.uint8)
        small = self._open(tmp_path, 4096, size=size, table_size=table)
        assert not small.put(key, value)
        with pytest.raises(ValueError, match="2049 grains of 4096 bytes"):
            small.alloc_put_buffer(3, value.size)
        small.close()
        large = self._open(tmp_path, 32768, size=size, table_size=table)
        assert large.put(key, value)
        assert large.get(key, out)
        np.testing.assert_array_equal(out, value)
        pb = large.alloc_put_buffer(3, value.size)  # the sizes check follows
        assert pb.value.size == value.size          # the grain, too
        with pytest.raises(ValueError, match="grains of 32768 bytes"):
            large.alloc_put_buffer(3, 2047 * 32768)
        large.close()
        assert "do not fit" not in capfd.readouterr().err

    @pytest.mark.parametrize("created,other", [
        (c, o) for c in _GRAINS for o in _GRAINS + [8192] if o != c])
    def test_reopening_with_another_grain_fails_and_harms_nothing(
            self, tmp_path, capfd, created, other):
        kv = self._open(tmp_path, created)
        assert kv.put(_key("k"), _val("kept"))
        kv.close()
        capfd.readouterr()
        with pytest.raises(IOError) as e:
            self._open(tmp_path, other, force_destroy=False)
        # What the store does today: it does not find the store (the
        # metadata's place depends on the grain), refuses to format over a
        # file that is not new, and says so. The error names the grain asked.
        assert "grain_size={}".format(other) in str(e.value)
        assert "no store of" in capfd.readouterr().err
        kv = self._open(tmp_path, created, force_destroy=False)
        out = np.zeros(4, dtype=np.uint8)
        assert kv.get(_key("k"), out)
        assert out.tobytes() == b"kept"
        kv.close()

    @pytest.mark.parametrize("grain", _GRAINS)
    def test_reopening_with_the_same_grain_finds_the_data(self, tmp_path,
                                                         grain):
        kv = self._open(tmp_path, grain)
        assert kv.put(_key("k"), _val("kept"))
        kv.close()
        kv = self._open(tmp_path, grain, force_destroy=False)
        out = np.zeros(4, dtype=np.uint8)
        assert kv.get(_key("k"), out)
        assert out.tobytes() == b"kept"
        kv.close()

    def test_force_destroy_makes_a_new_store_of_the_new_grain(self, tmp_path):
        kv = self._open(tmp_path, 4096)
        assert kv.put(_key("k"), _val("old"))
        kv.close()
        kv = self._open(tmp_path, 512)
        out = np.zeros(3, dtype=np.uint8)
        assert not kv.get(_key("k"), out)
        kv.close()


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
    kv.close()


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

    def test_put_and_get_values_are_writable(self, store):
        key = _key("flags!!!")
        pb = store.alloc_put_buffer(8, 10)
        assert pb.value.flags.writeable
        assert store.put_buffer(key, pb)
        gb = store.alloc_get_buffer(8, 10)
        assert store.get_into(key, gb)
        assert gb.value.flags.writeable
        assert gb.value.dtype == np.uint8

    def test_writing_into_a_get_value_changes_nothing_stored(self, store):
        key, data = _key("private!"), _bytes_of(5000, 3)
        _zc_put(store, key, data)
        gb = store.alloc_get_buffer(8, 5000)
        assert store.get_into(key, gb)
        v = gb.value
        v[:] = 0xEE                       # scribble over the whole value
        assert (gb.value == 0xEE).all()   # same memory, so it is visible here
        # The store still holds the original ...
        np.testing.assert_array_equal(_zc_get(store, key, 5000), data)
        out = np.zeros(5000, dtype=np.uint8)
        assert store.get(key, out)
        np.testing.assert_array_equal(out, data)
        # ... and a later get into the same buffer (while v is alive, so into
        # a fresh one; after v is gone, into the scribbled one) returns it.
        assert store.get_into(key, gb)
        np.testing.assert_array_equal(gb.value, data)
        np.testing.assert_array_equal(v, 0xEE)  # v untouched by that get
        del v
        gc.collect()
        gb.value[:] = 0x11
        assert store.get_into(key, gb)
        np.testing.assert_array_equal(gb.value, data)


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
        kv.close()
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

    def test_the_largest_record_allocates(self, big_store):
        # big_store: the 2047-grain record needs a segment that holds it; the
        # 4 MiB store's 514-grain segment (max_value_bytes) does not.
        big_store.alloc_put_buffer(8, _max_value(8))
        big_store.alloc_get_buffer(8, _max_value(8))

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
            other.close()

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



W = 8  # the queue_depth the multi-get tests open their store with


@pytest.fixture
def qstore(tmp_path):
    path = str(tmp_path / "pyudepot_mget")
    kv = pyudepot.uDepot(file_name=path, size=STORE_SIZE, force_destroy=True,
                         queue_depth=W)
    yield kv
    kv.close()


def _mkey(i):
    return _key("mget_key_%04d" % i)


def _present(i):
    return i % 3 != 2  # every third key is never stored


def _mval(i):
    return _bytes_of(300, i)


def _fill(kv, n):
    for i in range(n):
        if _present(i):
            _zc_put(kv, _mkey(i), _mval(i))


class TestMultiGetInto:
    @pytest.mark.parametrize("n", [0, 1, 2, W - 1, W, W + 1, 4 * W, 200])
    def test_each_key_gets_its_own_result_and_value(self, qstore, n):
        _fill(qstore, max(n, 1))
        keys = [_mkey(i) for i in range(n)]
        bufs = [qstore.alloc_get_buffer(keys[i].size, 300) if keys else None
                for i in range(n)]
        ok = qstore.multi_get_into(keys, bufs)
        assert ok == [_present(i) for i in range(n)]
        for i in range(n):
            if _present(i):
                np.testing.assert_array_equal(bufs[i].value, _mval(i))
                # the same bytes get_into returns
                one = qstore.alloc_get_buffer(keys[i].size, 300)
                assert qstore.get_into(keys[i], one)
                np.testing.assert_array_equal(bufs[i].value, one.value)
            else:
                assert bufs[i].value.size == 0

    def test_a_batch_far_past_the_kernels_queue_floor_needs_the_window(
            self, tmp_path):
        # The kernel sizes an AIO context for at least 8 I/Os per possible
        # CPU whatever depth was asked for, so only a batch past that fills
        # the queue: with every get in flight at once, some get -EAGAIN.
        n = max(4096, 16 * (os.cpu_count() or 1) + 1024)
        kv = pyudepot.uDepot(file_name=str(tmp_path / "wide"),
                             size=64 * 1024 * 1024, force_destroy=True,
                             queue_depth=W)
        try:
            data = _bytes_of(100, 1)
            keys = [_mkey(i) for i in range(n)]
            for k in keys:
                _zc_put(kv, k, data)
            bufs = [kv.alloc_get_buffer(13, 100) for _ in keys]
            assert kv.multi_get_into(keys, bufs) == [True] * n
            for b in bufs:
                np.testing.assert_array_equal(b.value, data)
        finally:
            kv.close()

    def test_a_buffer_passed_twice_is_refused(self, qstore):
        _fill(qstore, 4)
        gb = qstore.alloc_get_buffer(13, 300)
        other = qstore.alloc_get_buffer(13, 300)
        with pytest.raises(ValueError, match="more than once"):
            qstore.multi_get_into([_mkey(0), _mkey(1), _mkey(3)],
                                  [gb, other, gb])
        # refused before anything ran: neither buffer holds a value
        assert gb.value.size == 0 and other.value.size == 0

    def test_bad_arguments_are_refused(self, qstore, tmp_path):
        _fill(qstore, 2)
        gb = qstore.alloc_get_buffer(13, 300)
        pb = qstore.alloc_put_buffer(13, 300)
        with pytest.raises(ValueError):
            qstore.multi_get_into([_mkey(0), _mkey(1)], [gb])
        with pytest.raises(ValueError):
            qstore.multi_get_into([_mkey(0)], [pb])
        with pytest.raises(ValueError):
            qstore.multi_get_into([_mkey(0).astype(np.int32)], [gb])
        with pytest.raises(ValueError):
            qstore.multi_get_into([_mkey(0)], [None])
        foreign = pyudepot.uDepot(file_name=str(tmp_path / "other"),
                                  size=STORE_SIZE, force_destroy=True)
        try:
            with pytest.raises(ValueError):
                qstore.multi_get_into([_mkey(0)],
                                      [foreign.alloc_get_buffer(13, 300)])
        finally:
            foreign.close()

    def test_copies_no_value(self, qstore):
        _fill(qstore, 30)
        keys = [_mkey(i) for i in range(30)]
        bufs = [qstore.alloc_get_buffer(13, 300) for _ in keys]
        before = pyudepot.thread_value_copies()
        qstore.multi_get_into(keys, bufs)
        assert pyudepot.thread_value_copies() == before

    def test_a_live_array_is_not_overwritten_or_freed(self, qstore):
        _fill(qstore, 2)
        a = qstore.alloc_get_buffer(13, 300)
        b = qstore.alloc_get_buffer(13, 300)
        assert qstore.multi_get_into([_mkey(0), _mkey(1)], [a, b]) == \
            [True, True]
        held = a.value                      # a live array over a's buffer
        assert qstore.multi_get_into([_mkey(1), _mkey(0)], [a, b]) == \
            [True, True]
        np.testing.assert_array_equal(held, _mval(0))   # not overwritten
        np.testing.assert_array_equal(a.value, _mval(1))
        np.testing.assert_array_equal(b.value, _mval(0))

    def test_a_failed_key_leaves_an_empty_value(self, qstore):
        _fill(qstore, 3)
        a = qstore.alloc_get_buffer(13, 300)
        b = qstore.alloc_get_buffer(13, 300)
        assert qstore.multi_get_into([_mkey(0), _mkey(1)], [a, b]) == \
            [True, True]
        assert qstore.multi_get_into([_mkey(1), _mkey(2)], [a, b]) == \
            [True, False]
        assert a.value.size == 300 and b.value.size == 0

    def test_queue_depth_is_the_open_parameter(self, tmp_path, capfd):
        # A depth the kernel cannot give fails the open, saying so.
        with pytest.raises(IOError):
            pyudepot.uDepot(file_name=str(tmp_path / "huge"), size=STORE_SIZE,
                            force_destroy=True, queue_depth=2**31)
        assert "failed to init uDepot-ng" in capfd.readouterr().err

    def test_a_failure_that_is_not_a_missing_key_raises(self, qstore):
        _fill(qstore, 2)
        empty = np.empty(0, dtype=np.uint8)  # the store refuses it: -EINVAL
        a, b, c = (qstore.alloc_get_buffer(13, 300) for _ in range(3))
        with pytest.raises(IOError) as e:
            qstore.multi_get_into([_mkey(2), empty, _mkey(0)], [a, b, c])
        msg = str(e.value)
        assert "key 1" in msg and "Invalid argument" in msg
        assert e.value.errno == 22
        assert isinstance(e.value, pyudepot.MultiGetError)
        assert e.value.failed == [(1, -errno.EINVAL)]
        np.testing.assert_array_equal(c.value, _mval(0))  # the others read
        # every failed key is listed, in batch order, a missing one is not
        bufs = [qstore.alloc_get_buffer(13, 300) for _ in range(5)]
        with pytest.raises(IOError) as e:
            qstore.multi_get_into([empty, _mkey(2), _mkey(0), empty.copy(),
                                   _mkey(1)], bufs)
        assert e.value.failed == [(0, -errno.EINVAL), (3, -errno.EINVAL)]
        assert "key 0" in str(e.value) and "2 of 5" in str(e.value)
        # a missing key alone is not an error
        assert qstore.multi_get_into([_mkey(2)], [a]) == [False]

    def test_the_window_follows_the_thread_not_the_store(self, tmp_path):
        # A thread's AIO context is shared by every store on it, and the
        # store opened last sizes it. Opening a shallow store here shrinks
        # the context the deep store's gets run in: a window of the deep
        # store's own depth would fail the gets past it with -EAGAIN.
        n = max(4096, 16 * (os.cpu_count() or 1) + 1024)
        deep = pyudepot.uDepot(file_name=str(tmp_path / "deep"),
                               size=64 * 1024 * 1024, force_destroy=True)
        shallow = None
        try:
            data = _bytes_of(100, 2)
            keys = [_mkey(i) for i in range(n)]
            for k in keys:
                _zc_put(deep, k, data)
            shallow = pyudepot.uDepot(file_name=str(tmp_path / "shallow"),
                                      size=STORE_SIZE, force_destroy=True,
                                      queue_depth=W)
            bufs = [deep.alloc_get_buffer(13, 100) for _ in keys]
            assert deep.multi_get_into(keys, bufs) == [True] * n
        finally:
            if shallow:
                shallow.close()
            deep.close()


def _device_for(segments, segment_size, grain):
    """A device of `segments` segments and a tail of two grains for the
    device metadata (an exact multiple has none: the store would halve the
    segment). 64 segments or more: with fewer, salsa's spare segments halve
    it too."""
    return segments * segment_size + 2 * grain


class TestSegmentSize:
    """segment_size is the caller's, in bytes: the store's segment, and with
    it the tables (each fills one segment) and the longest value."""

    def _open(self, tmp_path, segment_size, grain=4096, segments=64,
              **kwargs):
        if "size" not in kwargs:
            kwargs["size"] = _device_for(segments, segment_size, grain)
        kwargs.setdefault("force_destroy", True)
        return pyudepot.uDepot(file_name=str(tmp_path / "segs"),
                               grain_size=grain, segment_size=segment_size,
                               **kwargs)

    @pytest.mark.parametrize("grain,segment", [
        (4096, 256 * 4096),
        (4096, 1100 * 4096),  # not a power of two of grains
        (4096, 3 * 4096),     # the least
        (512, 100 * 4096),
        (32768, 64 * 32768),
        (32768, 3 * 32768),   # the least
    ])
    def test_the_segment_reaches_the_store_and_bounds_the_value(
            self, tmp_path, capfd, grain, segment):
        kv = self._open(tmp_path, segment, grain,
                        segments=68 if segment > 2**20 else 64)
        assert kv.segment_size() == segment, \
            "the store halved it: " + capfd.readouterr().err
        assert kv.grain_size() == grain
        for key_size in (1, 8, 100):
            # seg_md_grains_ (store.cc) is one grain; a record is at most
            # 2047 grains; 14-byte header and 2-byte CRC around key and value.
            n = (min(2047, segment // grain - 1) * grain - 16 - key_size)
            assert kv.max_value_bytes(key_size) == n
        n = kv.max_value_bytes(8)
        key = np.full(8, 7, dtype=np.uint8)
        val = _bytes_of(n, 5)
        assert kv.put(key, val)
        out = np.zeros(n, dtype=np.uint8)
        assert kv.get(key, out)
        np.testing.assert_array_equal(out, val)
        key2 = np.full(8, 9, dtype=np.uint8)
        assert not kv.put(key2, np.zeros(n + 1, dtype=np.uint8))
        assert kv.exists(key2) is None
        kv.alloc_put_buffer(8, n)  # the buffer path agrees
        kv.close()

    def test_none_is_the_segment_of_the_table(self, tmp_path):
        a = pyudepot.uDepot(file_name=str(tmp_path / "a"), size=STORE_SIZE,
                            force_destroy=True)
        b = pyudepot.uDepot(file_name=str(tmp_path / "b"), size=STORE_SIZE,
                            force_destroy=True, segment_size=None)
        assert a.segment_size() == b.segment_size()
        a.close()
        b.close()

    @pytest.mark.parametrize("segment", [0, -4096, 2**64, 2**64 + 4096,
                                         4096.0, "4096", True, b"4096"])
    def test_a_segment_that_is_not_a_positive_64_bit_int_is_a_ValueError(
            self, tmp_path, segment):
        with pytest.raises(ValueError, match="segment_size="):
            self._open(tmp_path, segment, size=STORE_SIZE)
        assert not (tmp_path / "segs").exists()

    @pytest.mark.parametrize("table_size", [0, 4096, 1 << 20, 1 << 29])
    def test_a_segment_and_a_table_size_is_a_ValueError(self, tmp_path,
                                                        table_size):
        with pytest.raises(ValueError, match="both segment_size=1048576 and "
                                            "table_size={}".format(table_size)):
            self._open(tmp_path, 256 * 4096, table_size=table_size)
        assert not (tmp_path / "segs").exists()

    def test_the_default_table_size_passed_with_a_segment_is_a_conflict(
            self, tmp_path):
        # Passing table_size at all is the conflict, whatever its value.
        with pytest.raises(ValueError, match="both segment_size="):
            self._open(tmp_path, 256 * 4096,
                       table_size=pyudepot.DEFAULT_TABLE_SIZE)
        assert not (tmp_path / "segs").exists()

    @pytest.mark.parametrize("grain,segment,msg", [
        (4096, 4096 * 300 + 512, "segment size {} is not a multiple of "
                                 "4096 bytes"),
        (32768, 40000, "segment size {} is not a multiple of 32768 bytes"),
        # whole grains but not whole pages: the store would round it down
        (512, 512 * 33, "segment size {} is not a multiple of 4096 bytes"),
        # too small: one page, two (the least is a metadata grain and two of
        # table room, 3 grains)
        (4096, 4096, "segment size {} is too small for grains of 4096 "
                     "bytes: it must hold the segment's metadata grain and "
                     "a directory table's header, footer and buckets, at "
                     "least 12288 bytes"),
        (4096, 2 * 4096, "segment size {} is too small .* at least 12288 "
                         "bytes"),
        (32768, 2 * 32768, "segment size {} is too small .* at least 98304 "
                           "bytes"),
    ])
    def test_a_segment_the_store_would_change_or_cannot_hold_is_refused(
            self, tmp_path, capfd, grain, segment, msg):
        with pytest.raises(IOError) as e:
            self._open(tmp_path, segment, grain, size=64 * 1024 * 1024)
        assert "segment_size={}".format(segment) in str(e.value)
        assert re.search(msg.format(segment), capfd.readouterr().err), msg
        assert not (tmp_path / "segs").exists(), \
            "a refused segment must not create the file"

    def test_the_least_segment_opens_and_one_page_under_it_does_not(
            self, tmp_path):
        kv = self._open(tmp_path, 3 * 4096)
        assert kv.put(_key("k"), _val("v"))
        kv.close()
        with pytest.raises(IOError):
            self._open(tmp_path, 2 * 4096)

    def test_reopening_keeps_the_created_segment_and_ignores_the_argument(
            self, tmp_path, capfd):
        created = 256 * 4096
        size = _device_for(64, 512 * 4096, 4096)
        kv = self._open(tmp_path, created, size=size)
        assert kv.put(_key("k"), _val("kept"))
        kv.close()
        capfd.readouterr()
        for asked in (created, 512 * 4096, 64 * 4096, None):
            kv = self._open(tmp_path, asked, size=size, force_destroy=False)
            assert kv.segment_size() == created
            out = np.zeros(4, dtype=np.uint8)
            assert kv.get(_key("k"), out)
            assert out.tobytes() == b"kept"
            kv.close()
        # A table size does not reopen a store to another segment either.
        kv = pyudepot.uDepot(file_name=str(tmp_path / "segs"), size=size,
                             table_size=1 << 20)
        assert kv.segment_size() == created
        kv.close()
        # The argument is checked on a reopen too.
        with pytest.raises(IOError):
            self._open(tmp_path, 4096 * 300 + 512, size=size,
                       force_destroy=False)
        assert "is not a multiple of 4096 bytes" in capfd.readouterr().err
        kv = self._open(tmp_path, created, size=size, force_destroy=False)
        out = np.zeros(4, dtype=np.uint8)
        assert kv.get(_key("k"), out)
        kv.close()

    def test_a_device_too_small_for_the_segment_halves_it_and_says_so(
            self, tmp_path, capfd):
        asked = 1100 * 4096
        kv = self._open(tmp_path, asked, size=16 * 1024 * 1024)
        err = capfd.readouterr().err
        assert kv.segment_size() < asked
        assert "the requested segments of 4400 KiB" in err
        assert "Using segments of {} KiB".format(kv.segment_size() // 1024) \
            in err
        kv.close()


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
        kv.close()

    def test_grain_size_argument_is_what_the_store_reports(self, tmp_path):
        """grain_size=8192 with a table that holds a full record: the store
        reports that grain (read from C++, not echoed from the argument) and
        its largest value follows from the store's own segment."""
        grain = 8192
        table_size = 16 * 1024 * 1024
        # 32 segments of the table plus room for the per-segment metadata.
        kv = pyudepot.uDepot(file_name=str(tmp_path / "g8k"),
                             size=33 * (table_size + 64 * 1024),
                             force_destroy=True, table_size=table_size,
                             grain_size=grain)
        assert kv.grain_size() == grain
        # seg_md_grains_ (src/udepot/store.cc:1193) is
        # align_up(sizeof(salsa_seg_md), grain) / grain; salsa_seg_md is 53
        # bytes (extern/salsa/.../SalsaMD.hh:29), so one grain. A record gets
        # the segment less that (store.cc max_record_grains, ~2255), capped
        # at 2047 grains (HashEntry::kKvSizeMask).
        seg_md_grains = 1
        grains = min(2047, kv.segment_size() // grain - seg_md_grains)
        assert grains == 2047  # the table was chosen to make the cap bind
        # 14-byte header + 8-byte key + 2-byte CRC around the value.
        assert kv.max_value_bytes(8) == grains * grain - 16 - 8
        kv.close()

    def test_invalid_key_sizes_hold_no_value(self, store):
        assert store.max_value_bytes(0) == 0
        assert store.max_value_bytes(65536) == 0
        assert store.max_value_bytes(1) > 0
