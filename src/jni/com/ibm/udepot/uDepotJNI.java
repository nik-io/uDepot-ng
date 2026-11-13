// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

package com.ibm.udepot;

/**
 * JNI bindings for uDepot-ng key-value store.
 *
 * API-compatible with legacy uDepot for YCSB integration.
 */
public class uDepotJNI {
  /**
   * Initialize the Key Value provider, with the store's default segment.
   * @param fname    Path to file/device for backend storage.
   * @param size     Desired capacity in bytes. 0 for full capacity.
   * @param force_destroy If true, start a new store over whatever is there.
   *     If false, a store is restored, and a backing with no store on it is
   *     refused (a nonzero return) unless it is a new or empty file.
   * @return 0 on success, errno on failure.
   */
  public int init(String fname, long size, boolean force_destroy) {
    return initWithSegment(fname, size, force_destroy, 0);
  }

  /**
   * Initialize the Key Value provider. The store's parameters are the device
   * size, the grain size (fixed at 512 bytes here) and the segment size;
   * there is no table size: each index table fills one segment, so the
   * segment sets the tables (the largest power of two that fits it).
   * @param fname    Path to file/device for backend storage.
   * @param size     Desired capacity in bytes. 0 for full capacity.
   * @param force_destroy If true, start a new store over whatever is there.
   *     If false, a store is restored, and a backing with no store on it is
   *     refused (a nonzero return) unless it is a new or empty file.
   * @param segment_size Bytes of each segment: a multiple of 4096, or 0 for
   *     the store's default (a segment for a 2^29-byte table, halved until it
   *     fits the device, which the store says). A device too small for it
   *     makes the store halve it and say so. A reopen takes the segment the
   *     store was created with.
   * @return 0 on success, errno on failure (EINVAL for a bad segment_size,
   *     with the reason on stderr).
   */
  public native int initWithSegment(String fname, long size,
                                    boolean force_destroy, long segment_size);

  /**
   * Gracefully shutdown the Key Value provider.
   * @return 0 on success, errno on failure.
   */
  public native int shutdown();

  /**
   * Lookup a key and return its value.
   * @param key      Key byte array.
   * @param key_size Number of bytes in key.
   * @param value    Buffer to receive the value.
   * @param val_size Size of value buffer.
   * @return Number of bytes read on success, negative errno on failure.
   */
  public native int get(byte[] key, long key_size, byte[] value, long val_size);

  /**
   * Insert a key-value pair.
   * @param key      Key byte array.
   * @param key_size Number of bytes in key.
   * @param value    Value byte array.
   * @param val_size Number of bytes in value.
   * @return 0 on success, errno on failure.
   */
  public native int put(byte[] key, long key_size, byte[] value, long val_size);

  /**
   * Delete a key-value pair.
   * @param key      Key byte array.
   * @param key_size Number of bytes in key.
   * @return 0 on success, errno on failure.
   */
  public native int del(byte[] key, long key_size);

  /**
   * Returns the total size in bytes of stored key-value pairs.
   */
  public native long getSize();

  /**
   * Returns the total capacity of the underlying device in bytes.
   */
  public native long getRawDeviceCapacity();

  static {
    System.loadLibrary("uDepotJNI");
  }
}
