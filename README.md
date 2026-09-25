# uDepot Next Generation (uDepot-ng)

A ground-up rewrite of [uDepot](https://github.com/nik-io/uDepot) — a
multi-threaded, scalable, persistent store that is flash optimized by using
a log-structured space allocation and GC framework.

It uses a two-level directory map table as the main data structure
that grows together with the data and will utilize as much capacity as
possible before returning out of space.

See the [FAST19 paper](https://www.usenix.org/system/files/fast19-kourtis.pdf) for more details on uDepot. The log-structured space
allocation and GC is described in the [MASCOTS18 paper](https://ieeexplore.ieee.org/document/8526893).

### What changed from uDepot

- **Userspace RCU** replaces per-bucket mutexes — lock-free reads, per-thread
  epoch with zero shared-line atomic RMW on the read path
- **Eager-start C++23 coroutines** replace TRT — `initial_suspend = suspend_never`,
  so creating a CoroTask immediately submits I/O; batch N coroutines then
  harvest completions for queue-depth scaling
- **CMake** replaces the Makefile build
- **No TRT dependency** — standalone runtime, no separate scheduler
- **SPDK backend** — NVMe direct access via SPDK queue pairs, NVMe-oF
  support via `UDEPOT_NVMEF` env var

Everything else — on-disk format, hash function (CityHash64), segment
geometry, salsa allocator, I/O backend structure — is preserved from uDepot.

## Install

You need a C++23 compatible compiler (gcc >= 13 or clang >= 17), CMake >= 3.25,
and libaio. In Ubuntu:
```
$ apt-get install build-essential cmake libaio-dev -y
```

After git clone:

```
$ git submodule init
$ git submodule update
$ cmake -B build -DCMAKE_BUILD_TYPE=Release
$ cmake --build build -j$(nproc)
```

### Building with SPDK support

To build with the SPDK NVMe backend:

```
$ cd extern/spdk
$ git submodule update --init
$ ./configure
$ make -j$(nproc)
$ cd ../..
$ cmake -B build -DCMAKE_BUILD_TYPE=Release -DUDEPOT_BUILD_SPDK=ON
$ cmake --build build -j$(nproc)
```

## C++ usage example

```cpp
#include "udepot/store.h"

using udepot::PosixIO;
using udepot::UDepot;
using udepot::StoreConfig;

int main() {
    StoreConfig config;
    config.path = "/tmp/udepot-store";
    config.size = 64 * 1024 * 1024;
    config.grain_size = 512;

    UDepot<PosixIO> store;
    int rc = store.open(config);
    assert(rc == 0);

    std::string key = "hello";
    std::string val = "world";
    rc = store.put(key, val).run_sync();
    assert(rc == 0);

    uint8_t buf[64];
    size_t val_size = 0;
    rc = store.get(key, buf, sizeof(buf), &val_size).run_sync();
    assert(rc == 0);

    store.close();
}
```

### Async I/O with AIO backend

```cpp
#include "udepot/store.h"
#include "udepot/io/aio.h"

using udepot::AioIO;
using udepot::UDepot;
using udepot::CoroTask;

// Batch N operations for queue-depth scaling:
// each CoroTask eagerly submits its I/O at creation time.
std::vector<CoroTask<int>> tasks;
for (auto& key : keys)
    tasks.push_back(store.get(key, buf, buf_size, &val_size));

// All I/Os are in flight — now harvest completions.
for (auto& task : tasks)
    int rc = task.run_sync();
```

## Tests

```
$ ctest --test-dir build
```

12 test suites covering the coroutine runtime, RCU, hash table, directory,
both I/O backends, the full KV API, concurrent correctness, and async
queue-depth scaling.

### Using a block device

uDepot-ng sizes the store from `StoreConfig::size`. On a **regular file** the
store creates or truncates the file to that size. A **block device cannot be
truncated**, so set `size` to the device capacity.

**Device size must not be an exact multiple of the segment size.** uDepot puts
its device metadata in the tail left over after `align_down(device_size,
segment_size * grain_size)`. A device whose size divides exactly leaves no
tail, and init fails. This is why file-backed examples use sizes like
`64 * 1024 * 1024 + 1`.

Note the grain size: with an O_DIRECT backend every write must be a multiple
of the device sector size, and uDepot-ng sizes its segment metadata writes in
grains. A grain smaller than the sector size makes those writes fail with
`EINVAL`. Use `grain_size = 512` or `4096`; small grain sizes work only
against `/dev/shm` or other buffered backends.

## Notes

- Crash recovery is not yet implemented.

- Best performance is expected when using the AIO backend with batched
  coroutines for queue-depth scaling.

## Roadmap

- **Crash recovery** — persist and restore from the uDepot data log
- **io_uring backend** — kernel-side I/O submission ring
- **Network backends** — memcache protocol server (TCP/RDMA), NVMe over Fabrics
- **Python API** — ctypes bindings to `libpyudepot.so`
- **Java JNI API** — JNI bindings and YCSB benchmark integration
- **Directory grow** — deferred reclaim of retired directories, gradual per-table growth

## License

This project is licensed under the BSD 3-Clause License.
If you would like to see the detailed LICENSE click [here](LICENSE).
