# Copyright (c) 2024-2026 Nikolas Ioannou
# SPDX-License-Identifier: BSD-3-Clause

import ctypes
from ctypes import c_char_p, c_int, c_ubyte, c_uint, c_ulonglong, c_void_p, byref, POINTER
import errno
import os
import logging
import atexit
import weakref

import numpy as np
from numpy.ctypeslib import ndpointer

_lib_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         'libpyudepot.so')
libudepot = ctypes.cdll.LoadLibrary(
    _lib_path if os.path.exists(_lib_path) else 'libpyudepot.so')

pyopen = libudepot.uDepotOpenQueue
pyclose = libudepot.uDepotClose
pyget = libudepot.uDepotGet
pyput = libudepot.uDepotPut
pydel = libudepot.uDepotDel
pyexists = libudepot.uDepotExists

pyopen.argtypes = [c_char_p, c_ulonglong, c_int, c_ulonglong, c_uint]
pyopen.restype = c_void_p
pyclose.argtypes = [c_void_p]
pyget.argtypes = [c_void_p,
                  ndpointer(c_ubyte, flags="C_CONTIGUOUS"), c_uint,
                  ndpointer(c_ubyte, flags="C_CONTIGUOUS"), c_ulonglong]
pyput.argtypes = [c_void_p,
                  ndpointer(c_ubyte, flags="C_CONTIGUOUS"), c_uint,
                  ndpointer(c_ubyte, flags="C_CONTIGUOUS"), c_ulonglong]
pydel.argtypes = [c_void_p,
                  ndpointer(c_ubyte, flags="C_CONTIGUOUS"), c_uint]
pyexists.argtypes = [c_void_p,
                     ndpointer(c_ubyte, flags="C_CONTIGUOUS"), c_uint,
                     POINTER(c_ulonglong)]
pyexists.restype = c_int

pyallocput = libudepot.uDepotAllocPutBuffer
pyputvalue = libudepot.uDepotPutBufferValue
pyputbuf = libudepot.uDepotPutBuf
pyfreeput = libudepot.uDepotFreePutBuffer
pyallocget = libudepot.uDepotAllocGetBuffer
pynewget = libudepot.uDepotNewGetBuffer
pygetbuf = libudepot.uDepotGetBuf
pygetvalue = libudepot.uDepotGetBufferValue
pyfreeget = libudepot.uDepotFreeGetBuffer
pycopies = libudepot.uDepotThreadValueCopies
pymultiget = libudepot.uDepotMultiGetBuf

pyallocput.argtypes = [c_void_p, c_uint, c_ulonglong]
pyallocput.restype = c_void_p
pyputvalue.argtypes = [c_void_p, POINTER(c_ulonglong)]
pyputvalue.restype = c_void_p
pyputbuf.argtypes = [c_void_p,
                     ndpointer(c_ubyte, flags="C_CONTIGUOUS"), c_uint,
                     c_void_p]
pyputbuf.restype = c_int
pyfreeput.argtypes = [c_void_p]
pyfreeput.restype = None
pyallocget.argtypes = [c_void_p, c_uint, c_ulonglong]
pyallocget.restype = c_void_p
pynewget.argtypes = []
pynewget.restype = c_void_p
pygetbuf.argtypes = [c_void_p,
                     ndpointer(c_ubyte, flags="C_CONTIGUOUS"), c_uint,
                     c_void_p]
pygetbuf.restype = c_int
pygetvalue.argtypes = [c_void_p, POINTER(c_ulonglong)]
pygetvalue.restype = c_void_p
pyfreeget.argtypes = [c_void_p]
pyfreeget.restype = None
pycopies.argtypes = []
pycopies.restype = c_ulonglong
pymultiget.argtypes = [c_void_p, c_uint, c_void_p, c_void_p, c_void_p,
                       c_void_p]
pymultiget.restype = c_int


def thread_value_copies():
    """Values the calling thread's operations copied between caller memory
    and uDepot's buffers: one per copying put or get, none for a zero-copy
    one (put_buffer, get_into)."""
    return pycopies()


# The C buffer is freed through these, looked up when it is freed, so a test
# can count the calls.
def _free_put(handle):
    pyfreeput(handle)


def _free_get(handle):
    pyfreeget(handle)


class _Owned:
    """Owns a C buffer handle and frees it, exactly once, when collected.

    Every array handed out over the buffer reaches this object (array ->
    _Region -> _Owned), so the buffer outlives the PutBuffer or GetBuffer
    that was dropped while an array is still in use.
    """
    __slots__ = ('handle', 'regions', '__weakref__')

    def __init__(self, handle, free):
        self.handle = handle
        # The arrays alive over this buffer, held weakly.
        self.regions = weakref.WeakSet()
        fin = weakref.finalize(self, free, handle)
        fin.atexit = False  # the process frees its memory itself

    def in_use(self):
        return len(self.regions) > 0


class _Region:
    """A buffer's value as numpy sees it (__array_interface__).

    numpy makes this the base of the array returned by `.value`. A view of
    that array holds the array it was made from (its `.base` is that array
    or, on newer numpy, the object beneath it), so a view keeps the region,
    and through it the buffer, alive (checked by
    test_views_keep_the_buffer_alive on slices, reshapes and frombuffer).
    """
    __slots__ = ('__array_interface__', 'owned', '__weakref__')

    def __init__(self, owned, ptr, size, readonly):
        self.__array_interface__ = {
            'version': 3, 'shape': (size,), 'typestr': '|u1',
            'data': (ptr, readonly)}
        self.owned = owned
        owned.regions.add(self)


def _value_array(owned, ptr, size, readonly):
    if size == 0:  # nothing to view, so nothing to keep alive
        v = np.empty(0, dtype=np.uint8)
        v.flags.writeable = not readonly
        return v
    return np.asarray(_Region(owned, ptr, size, readonly))


class PutBuffer:
    """A buffer for one value, laid out as uDepot's on-disk record.

    Fill `value` in place, then store.put_buffer(key, buffer). The same
    buffer can be filled and put again once the put returns. A PutBuffer
    serves one call at a time, as the C++ buffers do; using one buffer from
    two threads at once is undefined. Arrays taken from `.value` may be
    shared between threads. The memory is the AIO
    backend's aligned_alloc memory (this binding is AIO-only); on SPDK it
    would be rte_malloc DMA memory tied to the SPDK environment, which the
    binding does not expose.

    `value` is a writable uint8 array viewing the buffer. It is NOT aligned
    beyond the record layout: the buffer is 4096-aligned and the value starts
    sizeof(KvHeader) + key_size = 14 + key_size bytes into it, so its address
    is (14 + key_size) mod 4096 within a page. The record's checksum (2
    bytes) follows the value, then zero padding to the 4096-byte grain; none
    of that is in `value`. An array outlives the PutBuffer: it keeps the
    buffer alive until it and every view of it are gone.
    """
    __slots__ = ('_store', '_owned', 'key_size', 'val_size')

    def __init__(self, store, owned, key_size, val_size):
        self._store = store
        self._owned = owned
        self.key_size = key_size
        self.val_size = val_size

    @property
    def value(self):
        size = c_ulonglong(0)
        ptr = pyputvalue(self._owned.handle, byref(size))
        return _value_array(self._owned, ptr, size.value, False)


class GetBuffer:
    """A buffer for get_into() to read values into, reused across gets.

    `value` is a writable uint8 array viewing the value of the last
    successful get_into, empty before one and after a failed one. Like a
    PutBuffer's it is unaligned (starts 14 + key_size bytes into a
    4096-aligned record buffer, key_size being the key just read).

    A get reads into the buffer when the record fits, else into a larger
    one that replaces it, so take `value` after each get. An array taken
    earlier keeps its bytes: while one is alive the next get_into reads into
    a new buffer instead of overwriting or freeing that one (an allocation
    the reuse would have saved; drop the array first to avoid it). A GetBuffer
    serves one call at a time, as the C++ buffers do; using one buffer from
    two threads at once is undefined. Arrays taken from `.value` may be
    shared between threads: a later get_into never overwrites or frees a
    buffer that an array still views, so the buffer is private to the arrays
    made from it: writing into `value` changes nothing in the store and
    nothing a later get returns (it is writable so that consumers such as
    torch.from_numpy need not copy). The memory is the AIO backend's aligned_alloc memory
    (this binding is AIO-only); on SPDK it would be rte_malloc DMA memory
    tied to the SPDK environment, which the binding does not expose.
    """
    __slots__ = ('_store', '_owned', '_has_value')

    def __init__(self, store, owned):
        self._store = store
        self._owned = owned
        self._has_value = False

    @property
    def value(self):
        if not self._has_value:
            return _value_array(self._owned, None, 0, False)
        size = c_ulonglong(0)
        ptr = pygetvalue(self._owned.handle, byref(size))
        return _value_array(self._owned, ptr, size.value, False)


# Index tables of 2 MiB: small, for tests. A store sized for real use
# passes its own table_size (a power of two of at least 4096 bytes, or 0
# for the store's default, 512 MiB tables); tables this small map on
# 4 KiB pages and directory grows are slower, which the store prints.
DEFAULT_TABLE_SIZE = 2 * 1024 * 1024


class uDepot:
    def __init__(self, **kwargs):
        """Opens the store on file_name (a file or a block device).

        force_destroy=True starts a new store over whatever is there.
        force_destroy=False restores the store, and raises IOError for a
        backing with no store on it (one created with another size, say)
        unless it is a new or empty file.

        queue_depth is the number of I/Os each thread may have in flight
        (the size of its AIO context; 0, the default, is 1024); multi_get_into
        keeps at most that many gets in flight on the calling thread, fewer
        if that thread's AIO context has less room.
        """
        self._fname = kwargs.get('file_name', '/tmp/pyudepot-test')
        self._size = kwargs.get('size', 1024 * 1024 + 4096)
        self._force_destroy = 1 if kwargs.get('force_destroy', False) else 0
        self._table_size = kwargs.get('table_size', DEFAULT_TABLE_SIZE)
        self._queue_depth = kwargs.get('queue_depth', 0)
        self._kv = pyopen(
            self._fname.encode('utf-8'), self._size, self._force_destroy,
            self._table_size, self._queue_depth)
        if not self._kv:
            raise IOError('failed to open uDepot for {}'.format(self._fname))
        atexit.register(self._cleanup)

    def _cleanup(self):
        # As uDepot's close: shut down and free the store. The caller must
        # ensure no call on this object is in progress or follows.
        if self._kv:
            pyclose(self._kv)
            self._kv = None

    def _handle(self):
        # Calls after close would pass a freed pointer to C.
        if not self._kv:
            raise ValueError('uDepot store is closed')
        return self._kv

    def get(self, key, val_out):
        rc = pyget(self._handle(), key, key.size, val_out, val_out.size)
        if rc != 0:
            logging.info('pyget returned=%d', rc)
            return False
        return True

    def put(self, key, val):
        rc = pyput(self._handle(), key, key.size, val, val.size)
        if rc != 0:
            logging.info('pyput returned=%d', rc)
            return False
        return True

    def alloc_put_buffer(self, key_size, val_size):
        """A PutBuffer for a key of key_size bytes and a value of val_size.

        Raises ValueError if the sizes cannot be stored (key 1..65535
        bytes; the record, 14 + key_size + val_size + 2 bytes, at most 2047
        grains of 4096 bytes) and MemoryError if the allocation fails.
        """
        handle = self._alloc(pyallocput, key_size, val_size, 'put')
        return PutBuffer(self, _Owned(handle, _free_put), key_size, val_size)

    def alloc_get_buffer(self, key_size, val_size):
        """A GetBuffer that holds a record of a key of key_size bytes and a
        value of up to val_size without growing. Raises as
        alloc_put_buffer."""
        handle = self._alloc(pyallocget, key_size, val_size, 'get')
        return GetBuffer(self, _Owned(handle, _free_get))

    def _alloc(self, alloc, key_size, val_size, what):
        kv = self._handle()
        if not (0 < key_size <= 0xffff and 0 <= val_size < 2**32 - 1):
            raise ValueError(
                'cannot allocate a {} buffer for key_size={} val_size={}: '
                'key_size must be 1..65535 and val_size below 2**32-1'
                .format(what, key_size, val_size))
        # The store's check_sizes: a record is at most 2047 grains (11 bits
        # of HashEntry's kv size); the binding's grain is 4096 bytes.
        grains = (14 + key_size + val_size + 2 + 4095) // 4096
        if grains > 2047:
            raise ValueError(
                'cannot allocate a {} buffer for key_size={} val_size={}: '
                'the record needs {} grains of 4096 bytes, the store holds '
                'at most 2047 per record'
                .format(what, key_size, val_size, grains))
        handle = alloc(kv, key_size, val_size)
        if not handle:
            raise MemoryError(
                'cannot allocate a {} buffer for key_size={} val_size={}: '
                'allocation failed (out of memory)'
                .format(what, key_size, val_size))
        return handle

    def _own(self, buf, kind):
        if not isinstance(buf, kind) or buf._store is not self:
            raise ValueError('not a {} of this store'.format(kind.__name__))
        return self._handle()

    def put_buffer(self, key, buf):
        """Stores the value in `buf` (a PutBuffer of this store) under key,
        copying nothing. Returns False if the store refuses, as put. Raises
        ValueError if key.size is not the key_size the buffer was allocated
        for."""
        kv = self._own(buf, PutBuffer)
        if key.size != buf.key_size:
            raise ValueError(
                'key has {} bytes, the PutBuffer was allocated for {}'
                .format(key.size, buf.key_size))
        rc = pyputbuf(kv, key, key.size, buf._owned.handle)
        if rc != 0:
            logging.info('pyputbuf returned=%d', rc)
            return False
        return True

    def get_into(self, key, buf):
        """Reads key's value into `buf` (a GetBuffer of this store); read it
        from buf.value. Returns False if the key is missing or the get
        fails, with buf.value empty."""
        kv = self._own(buf, GetBuffer)
        owned = self._get_target(buf)
        buf._has_value = False
        rc = pygetbuf(kv, key, key.size, owned.handle)
        if rc != 0:
            logging.info('pygetbuf returned=%d', rc)
            return False
        buf._owned = owned
        buf._has_value = True
        return True

    @staticmethod
    def _get_target(buf):
        """The C buffer a get into `buf` reads into: its own, or a new one
        if an earlier array still views it (which a get must not overwrite
        or free)."""
        owned = buf._owned
        if owned.in_use():
            handle = pynewget()
            if not handle:
                raise MemoryError('cannot allocate a get buffer')
            owned = _Owned(handle, _free_get)
        return owned

    def multi_get_into(self, keys, bufs):
        """get_into for every keys[i] and bufs[i], with the gets in flight
        together on this thread (one C call, no threads): up to the smaller of
        the store's queue_depth and this thread's AIO context room at once,
        each new one starting as another completes.
        Returns a list of bools, one per key: True with bufs[i].value the
        value of keys[i]; False for a key that is not in the store, with
        bufs[i].value empty. Any other failure (-EAGAIN, an I/O error) raises
        IOError naming the first such key and its error, after every other
        buffer has been filled: it is not a missing key, so it is not False.
        (get_into still returns False for every failure; making it raise is a
        separate change.)

        keys are C-contiguous uint8 arrays; bufs are GetBuffers of this
        store, all different (a buffer serves one get): ValueError otherwise.
        The buffers' lifetime rules are get_into's: one with a live array
        reads into a new C buffer.
        """
        kv = self._handle()
        n = len(keys)
        if len(bufs) != n:
            raise ValueError('{} keys but {} buffers'.format(n, len(bufs)))
        if len({id(b) for b in bufs}) != n:
            raise ValueError(
                'a GetBuffer appears more than once: one buffer serves one '
                'get, so each key needs its own')
        for k in keys:
            if (not isinstance(k, np.ndarray) or k.dtype != np.uint8
                    or k.ndim != 1 or not k.flags.c_contiguous):
                raise ValueError('a key is not a C-contiguous 1-d uint8 array')
        for b in bufs:
            self._own(b, GetBuffer)
        if n == 0:
            return []
        targets = [self._get_target(b) for b in bufs]
        for b in bufs:
            b._has_value = False
        c_keys = (c_void_p * n)(*[k.ctypes.data for k in keys])
        c_sizes = (c_uint * n)(*[k.size for k in keys])
        c_bufs = (c_void_p * n)(*[t.handle for t in targets])
        results = (c_int * n)()
        rc = pymultiget(kv, n, c_keys, c_sizes, c_bufs, results)
        if rc != 0:
            raise MemoryError('multi_get_into: out of memory (rc={})'
                              .format(rc))
        ok = []
        failed = []  # (index, rc) of the gets that failed for another reason
        for i in range(n):
            if results[i] == 0:
                bufs[i]._owned = targets[i]
                bufs[i]._has_value = True
                ok.append(True)
            else:
                if results[i] != -errno.ENOENT:
                    failed.append((i, results[i]))
                logging.info('pymultiget key %d returned=%d', i, results[i])
                ok.append(False)
        if failed:
            i, rc = failed[0]
            raise IOError(
                -rc, 'multi_get_into: {} of {} gets failed, not as a missing '
                'key; the first, key {}: {}. The buffers of the keys that '
                'succeeded hold their values; retry the failed ones'.format(
                    len(failed), n, i, os.strerror(-rc)))
        return ok

    def delete(self, key):
        rc = pydel(self._handle(), key, key.size)
        if rc != 0:
            logging.info('pydel returned=%d', rc)
            return False
        return True

    def exists(self, key):
        val_size = c_ulonglong(0)
        rc = pyexists(self._handle(), key, key.size, byref(val_size))
        if rc != 0:
            return None
        return val_size.value
