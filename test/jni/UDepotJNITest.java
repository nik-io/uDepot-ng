// Copyright (c) 2024-2026 Nikolas Ioannou
// SPDX-License-Identifier: BSD-3-Clause

import com.ibm.udepot.uDepotJNI;

import java.lang.Math;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

class UDepotJNITest {
    static {
        System.loadLibrary("uDepotJNI");
    }

    private uDepotJNI KV;
    private int passed = 0;
    private int failed = 0;

    private void check(boolean condition, String name) {
        if (condition) {
            passed++;
        } else {
            failed++;
            System.err.println("FAIL: " + name);
        }
    }

    UDepotJNITest(String fname, long size) {
        KV = new uDepotJNI();
        int rc = KV.init(fname, size, true);
        if (rc != 0)
            throw new RuntimeException("init failed: " + rc);
    }

    void shutdown() {
        if (KV != null) KV.shutdown();
        KV = null;
    }

    void testPutThenGet() {
        byte[] key = "hello".getBytes();
        byte[] val = "world".getBytes();
        int rc = KV.put(key, key.length, val, val.length);
        check(rc == 0, "put returns 0");

        byte[] buf = new byte[64];
        rc = KV.get(key, key.length, buf, buf.length);
        check(rc == val.length, "get returns value size");
        check(Arrays.equals(Arrays.copyOf(buf, rc), val),
              "get returns correct value");
    }

    void testGetNonexistentKey() {
        byte[] key = "nosuchkey_jni".getBytes();
        byte[] buf = new byte[64];
        int rc = KV.get(key, key.length, buf, buf.length);
        check(rc < 0, "get nonexistent returns negative");
    }

    void testDeleteRemovesKey() {
        byte[] key = "to_delete_jni".getBytes();
        byte[] val = "deleteme".getBytes();
        int rc = KV.put(key, key.length, val, val.length);
        check(rc == 0, "put for delete returns 0");

        rc = KV.del(key, key.length);
        check(rc == 0, "del returns 0");

        byte[] buf = new byte[64];
        rc = KV.get(key, key.length, buf, buf.length);
        check(rc < 0, "get after delete returns negative");
    }

    void testManyKeys() {
        int count = 200;
        for (int i = 0; i < count; i++) {
            byte[] key = ("jni_key_" + i).getBytes();
            byte[] val = ("jni_val_" + i).getBytes();
            int rc = KV.put(key, key.length, val, val.length);
            check(rc == 0, "put many key " + i);
        }

        for (int i = 0; i < count; i++) {
            byte[] key = ("jni_key_" + i).getBytes();
            byte[] expected = ("jni_val_" + i).getBytes();
            byte[] buf = new byte[64];
            int rc = KV.get(key, key.length, buf, buf.length);
            check(rc == expected.length, "get many size " + i);
            check(Arrays.equals(Arrays.copyOf(buf, Math.max(0, rc)), expected),
                  "get many value " + i);
        }
    }

    void testLargeValue() {
        byte[] key = "largekey_jni".getBytes();
        byte[] val = new byte[4096];
        Arrays.fill(val, (byte) 0xAB);
        int rc = KV.put(key, key.length, val, val.length);
        check(rc == 0, "put large value returns 0");

        byte[] buf = new byte[4096];
        rc = KV.get(key, key.length, buf, buf.length);
        check(rc == val.length, "get large value returns size");
        check(Arrays.equals(buf, val), "get large value matches");
    }

    void testGetRawDeviceCapacity() {
        long cap = KV.getRawDeviceCapacity();
        check(cap > 0, "getRawDeviceCapacity > 0");
    }

    void testConcurrentPutGet() throws InterruptedException {
        int threadCount = 4;
        int opsPerThread = 100;
        java.util.concurrent.atomic.AtomicInteger errors =
            new java.util.concurrent.atomic.AtomicInteger(0);

        Thread[] writers = new Thread[threadCount];
        for (int t = 0; t < threadCount; t++) {
            final int tid = t;
            writers[t] = new Thread(() -> {
                for (int i = 0; i < opsPerThread; i++) {
                    byte[] k = ("ct" + tid + "_k" + i).getBytes();
                    byte[] v = ("ct" + tid + "_v" + i).getBytes();
                    if (KV.put(k, k.length, v, v.length) != 0)
                        errors.incrementAndGet();
                }
            });
        }
        for (Thread w : writers) w.start();
        for (Thread w : writers) w.join();
        check(errors.get() == 0, "concurrent puts no errors");

        for (int t = 0; t < threadCount; t++) {
            for (int i = 0; i < opsPerThread; i++) {
                byte[] k = ("ct" + t + "_k" + i).getBytes();
                byte[] expected = ("ct" + t + "_v" + i).getBytes();
                byte[] buf = new byte[64];
                int rc = KV.get(k, k.length, buf, buf.length);
                check(rc == expected.length,
                      "concurrent get size t=" + t + " i=" + i);
            }
        }
    }

    // Regression: shutdown() deleted the store while other threads were in
    // get/put/del, which read the global pointer without the lock. Calls
    // racing or following shutdown() must now fail cleanly (-ESHUTDOWN).
    void testShutdownWhileThreadsRun() throws InterruptedException {
        final uDepotJNI kv = KV;
        final int eshutdown = 108;
        int threadCount = 4;
        java.util.concurrent.atomic.AtomicInteger ops =
            new java.util.concurrent.atomic.AtomicInteger(0);
        java.util.concurrent.atomic.AtomicInteger unexpected =
            new java.util.concurrent.atomic.AtomicInteger(0);

        Thread[] threads = new Thread[threadCount];
        for (int t = 0; t < threadCount; t++) {
            final int tid = t;
            threads[t] = new Thread(() -> {
                byte[] buf = new byte[64];
                for (int i = 0; ; i++) {
                    byte[] k = ("sd" + tid + "_k" + (i % 32)).getBytes();
                    int rc = (i % 2 == 0)
                        ? kv.put(k, k.length, k, k.length)
                        : kv.get(k, k.length, buf, buf.length);
                    ops.incrementAndGet();
                    if (rc == -eshutdown) break;
                    if (rc < 0 && rc != -2 /* ENOENT */)
                        unexpected.incrementAndGet();
                }
                byte[] k = ("sd" + tid + "_k0").getBytes();
                if (kv.del(k, k.length) != -eshutdown)
                    unexpected.incrementAndGet();
            });
        }
        for (Thread th : threads) th.start();
        while (ops.get() < 2000) Thread.yield();
        kv.shutdown();
        for (Thread th : threads) th.join();
        check(unexpected.get() == 0, "calls racing shutdown fail cleanly");
    }

    public static void main(String[] argv) throws Exception {
        String fname;
        if (argv.length > 0) {
            fname = argv[0];
        } else {
            fname = System.getProperty("java.io.tmpdir") +
                    "/udepot_jni_test";
        }

        long size = 4L * 1024 * 1024;
        System.out.println("JNI test with file=" + fname +
                           " size=" + (size >> 20) + " MiB");

        UDepotJNITest test = new UDepotJNITest(fname, size);
        try {
            test.testPutThenGet();
            test.testGetNonexistentKey();
            test.testDeleteRemovesKey();
            test.testManyKeys();
            test.testLargeValue();
            test.testGetRawDeviceCapacity();
            test.testConcurrentPutGet();
            test.testShutdownWhileThreadsRun();  // last: shuts the store
        } finally {
            test.shutdown();
            Files.deleteIfExists(Path.of(fname));
        }

        System.out.println("Passed: " + test.passed +
                           ", Failed: " + test.failed);
        if (test.failed > 0) System.exit(1);
    }
}
