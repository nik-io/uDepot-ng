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
   * Initialize the Key Value provider.
   * @param fname    Path to file/device for backend storage.
   * @param size     Desired capacity in bytes. 0 for full capacity.
   * @param force_destroy If true, assume empty device (no restore).
   * @return 0 on success, errno on failure.
   */
  public native int init(String fname, long size, boolean force_destroy);

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
