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
   * Index table size init() uses: 2 MiB, small, for tests. A store sized
   * for real use passes its own to initWithTables; tables this small map
   * on 4 KiB pages and directory grows are slower, which the store prints.
   */
  public static final long DEFAULT_TABLE_SIZE = 2L * 1024 * 1024;

  /**
   * Initialize the Key Value provider, with DEFAULT_TABLE_SIZE tables.
   * @param fname    Path to file/device for backend storage.
   * @param size     Desired capacity in bytes. 0 for full capacity.
   * @param force_destroy If true, assume empty device (no restore).
   * @return 0 on success, errno on failure.
   */
  public int init(String fname, long size, boolean force_destroy) {
    return initWithTables(fname, size, force_destroy, DEFAULT_TABLE_SIZE);
  }

  /**
   * Initialize the Key Value provider.
   * @param fname    Path to file/device for backend storage.
   * @param size     Desired capacity in bytes. 0 for full capacity.
   * @param force_destroy If true, assume empty device (no restore).
   * @param table_size Bytes of each index table: a power of two of at least
   *                 4096, or 0 for the store's default (512 MiB tables).
   * @return 0 on success, errno on failure (EINVAL for a bad table_size).
   */
  public native int initWithTables(String fname, long size,
                                   boolean force_destroy, long table_size);

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
