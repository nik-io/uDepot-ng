// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

#include "com_ibm_udepot_uDepotJNI.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "udepot/store.h"
#include "udepot/io/aio.h"

using Store = udepot::UDepot<udepot::AioIO>;

static Store* g_store = nullptr;
static std::mutex g_mtx;

JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_init(
    JNIEnv* env, jobject, jstring fname, jlong size, jboolean force_destroy) {
    (void)force_destroy;

    const char* path = env->GetStringUTFChars(fname, nullptr);
    if (!path) return ENOMEM;

    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_store) {
        env->ReleaseStringUTFChars(fname, path);
        return 0;
    }

    g_store = new (std::nothrow) Store();
    if (!g_store) {
        env->ReleaseStringUTFChars(fname, path);
        return ENOMEM;
    }

    udepot::StoreConfig config;
    config.path = path;
    config.size = static_cast<size_t>(size);
    config.grain_size = 512;
    config.initial_tables = 4;
    config.index_bits = 14;

    int rc = g_store->open(config);
    env->ReleaseStringUTFChars(fname, path);

    if (rc != 0) {
        delete g_store;
        g_store = nullptr;
    }
    return rc;
}

JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_shutdown(
    JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_store) {
        g_store->close();
        delete g_store;
        g_store = nullptr;
    }
    return 0;
}

JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_get(
    JNIEnv* env, jobject, jbyteArray key, jlong key_size,
    jbyteArray val, jlong val_size) {
    if (!g_store) return -EINVAL;

    auto* key_ptr = static_cast<jbyte*>(
        env->GetPrimitiveArrayCritical(key, nullptr));
    if (!key_ptr) return -ENOMEM;
    auto* val_ptr = static_cast<jbyte*>(
        env->GetPrimitiveArrayCritical(val, nullptr));
    if (!val_ptr) {
        env->ReleasePrimitiveArrayCritical(key, key_ptr, 0);
        return -ENOMEM;
    }

    auto key_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(key_ptr),
        static_cast<size_t>(key_size));

    size_t val_size_read = 0;
    int rc = g_store->get(key_span,
                          reinterpret_cast<uint8_t*>(val_ptr),
                          static_cast<size_t>(val_size),
                          &val_size_read).run_sync();

    env->ReleasePrimitiveArrayCritical(val, val_ptr, 0);
    env->ReleasePrimitiveArrayCritical(key, key_ptr, 0);

    if (rc != 0) return -std::abs(rc);
    return static_cast<jint>(val_size_read);
}

JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_put(
    JNIEnv* env, jobject, jbyteArray key, jlong key_size,
    jbyteArray val, jlong val_size) {
    if (!g_store) return EINVAL;

    auto* key_ptr = static_cast<jbyte*>(
        env->GetPrimitiveArrayCritical(key, nullptr));
    if (!key_ptr) return ENOMEM;
    auto* val_ptr = static_cast<jbyte*>(
        env->GetPrimitiveArrayCritical(val, nullptr));
    if (!val_ptr) {
        env->ReleasePrimitiveArrayCritical(key, key_ptr, 0);
        return ENOMEM;
    }

    auto key_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(key_ptr),
        static_cast<size_t>(key_size));
    auto val_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(val_ptr),
        static_cast<size_t>(val_size));

    int rc = g_store->put(key_span, val_span).run_sync();

    env->ReleasePrimitiveArrayCritical(val, val_ptr, 0);
    env->ReleasePrimitiveArrayCritical(key, key_ptr, 0);
    return rc;
}

JNIEXPORT jint JNICALL Java_com_ibm_udepot_uDepotJNI_del(
    JNIEnv* env, jobject, jbyteArray key, jlong key_size) {
    if (!g_store) return EINVAL;

    auto* key_ptr = static_cast<jbyte*>(
        env->GetPrimitiveArrayCritical(key, nullptr));

    auto key_span = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(key_ptr),
        static_cast<size_t>(key_size));

    int rc = g_store->del(key_span).run_sync();

    env->ReleasePrimitiveArrayCritical(key, key_ptr, 0);
    return rc;
}

JNIEXPORT jlong JNICALL Java_com_ibm_udepot_uDepotJNI_getSize(
    JNIEnv*, jobject) {
    if (!g_store) return 0;
    // TODO: track KV utilization bytes when crash recovery is added.
    return 0;
}

JNIEXPORT jlong JNICALL Java_com_ibm_udepot_uDepotJNI_getRawDeviceCapacity(
    JNIEnv*, jobject) {
    if (!g_store) return 0;
    return static_cast<jlong>(g_store->io().get_size());
}
