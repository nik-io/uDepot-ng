# Copyright (c) 2024-2026 Nikolas Ioannou
# SPDX-License-Identifier: BSD-3-Clause

import ctypes
from ctypes import c_char_p, c_int, c_ubyte, c_uint, c_ulonglong, c_void_p, byref, POINTER
import os
import logging
import atexit

import numpy as np
from numpy.ctypeslib import ndpointer

_lib_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         'libpyudepot.so')
libudepot = ctypes.cdll.LoadLibrary(
    _lib_path if os.path.exists(_lib_path) else 'libpyudepot.so')

pyopen = libudepot.uDepotOpen
pyclose = libudepot.uDepotClose
pyfree = libudepot.uDepotFree
pyget = libudepot.uDepotGet
pyput = libudepot.uDepotPut
pydel = libudepot.uDepotDel
pyexists = libudepot.uDepotExists

pyopen.argtypes = [c_char_p, c_ulonglong, c_int]
pyopen.restype = c_void_p
pyclose.argtypes = [c_void_p]
pyfree.argtypes = [c_void_p]
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


class uDepot:
    def __init__(self, **kwargs):
        self._fname = kwargs.get('file_name', '/tmp/pyudepot-test')
        self._size = kwargs.get('size', 1024 * 1024 + 4096)
        self._force_destroy = 1 if kwargs.get('force_destroy', False) else 0
        self._closed = False
        self._kv = pyopen(
            self._fname.encode('utf-8'), self._size, self._force_destroy)
        if not self._kv:
            raise IOError('failed to open uDepot for {}'.format(self._fname))
        atexit.register(self._cleanup)

    def _cleanup(self):
        # Close but keep the handle: a thread still inside a call on it must
        # not see the store freed, and later calls fail cleanly
        # (-ESHUTDOWN). The memory is released in __del__, when no thread
        # can hold a reference to this object any more.
        if self._kv and not self._closed:
            pyclose(self._kv)
            self._closed = True

    def __del__(self):
        kv = getattr(self, '_kv', None)
        if kv:
            self._cleanup()
            pyfree(kv)
            self._kv = None

    def get(self, key, val_out):
        rc = pyget(self._kv, key, key.size, val_out, val_out.size)
        if rc != 0:
            logging.info('pyget returned=%d', rc)
            return False
        return True

    def put(self, key, val):
        rc = pyput(self._kv, key, key.size, val, val.size)
        if rc != 0:
            logging.info('pyput returned=%d', rc)
            return False
        return True

    def delete(self, key):
        rc = pydel(self._kv, key, key.size)
        if rc != 0:
            logging.info('pydel returned=%d', rc)
            return False
        return True

    def exists(self, key):
        val_size = c_ulonglong(0)
        rc = pyexists(self._kv, key, key.size, byref(val_size))
        if rc != 0:
            return None
        return val_size.value
