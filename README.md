# KernQueue

A Linux character-device driver in C with a C++17 client, concurrency tests and reproducible QEMU benchmarks. Applications submit checksum jobs to `/dev/kernqueue` and receive results asynchronously through blocking I/O or `poll`/`epoll`.

This is a software device for studying kernel/userspace interfaces, synchronization and resource lifetime. It does not control hardware, implement DMA or accelerate checksums. FNV-1a is a demonstration checksum, not a cryptographic hash.

## Verified results

Passed all **14 functional scenarios** in isolated guests running **6.17.0-1022-azure** and **7.0.0-1017-azure**, including 2,000 shared-descriptor results with no losses/duplicates, 200 close-with-work cycles and five module reloads per guest.

On the 7.0 guest, five runs measured **57,585 verified jobs/s median** for one client and **178.46 µs p50 / 320.12 µs p95** round-trip latency over 1,000 samples. These are **QEMU TCG software-emulation measurements**, not native hardware or GPU throughput. Raw results, ranges and methods are in [docs/RESULTS.md](docs/RESULTS.md); earlier 6.17 evidence is preserved under [docs/evidence/kernel-6.17](docs/evidence/kernel-6.17).

## What it implements

- A `miscdevice` with a separate context for each `open`; duplicated/shared descriptors share the same context.
- Up to **64 accepted outstanding jobs per context**, including queued, executing and completed-but-unread jobs. Userspace must consume results to release slots.
- Fixed-size requests with **1–256-byte payloads**, caller-supplied 64-bit IDs and FNV-1a-64 results.
- An unbound kernel workqueue, mutex-protected lists, interruptible wait queues and blocking/nonblocking reads and writes.
- Readable/writable `poll` and `epoll` readiness, `EAGAIN` backpressure and explicit request validation.
- Checked user copies; an `EFAULT` on read leaves the result available for retry.
- Last-close cancellation and cleanup; module reference counting prevents unloading while descriptors remain open.
- A C++17 tool with functional scenarios, a multithreaded stress test and a five-run throughput/latency benchmark.

## Build and run in an isolated VM

Linux x86-64 is required for the supplied VM script. On Ubuntu:

```bash
sudo apt-get install build-essential linux-headers-$(uname -r) qemu-system-x86 busybox-static cpio python3
make vm
```

The script builds the module for the selected kernel, creates a minimal initramfs and runs a **networkless QEMU guest with software emulation**, four vCPUs and 512 MiB RAM. No module is loaded on the host. If the kernel image is root-readable only, the script uses `sudo -n` solely to copy it. Alternatively provide a readable `KERNEL_IMAGE`.

```bash
KERNEL_VERSION=6.17.0-1022-azure \
KDIR=/lib/modules/6.17.0-1022-azure/build \
KERNEL_IMAGE=/path/to/readable/vmlinuz make vm
```

A successful run prints `KERNQUEUE_VM_PASS` and writes the raw console transcript, environment/artifact hashes, benchmark JSON and [results report](docs/RESULTS.md). A 240-second deadline catches hangs. Software-emulated numbers depend strongly on host load and are not native-performance claims.

To build without running the guest:

```bash
make all
```

For an expendable Linux development machine or your own VM, manual usage is:

```bash
sudo insmod kernel/kernqueue.ko
sudo build/kqctl checksum hello   # a430d84680aabd0b
sudo build/kqctl test
sudo build/kqctl bench
sudo rmmod kernqueue
```

The device defaults to mode `0600`. Do not make this experimental interface world-writable on a shared host.

## Interface and architecture

`write(fd, &request, 272)` submits exactly one job. `read(fd, &result, 24)` consumes one completion. Larger read buffers are accepted but return one result; short reads and nonexact writes return `EINVAL`. The fixed layout is in [`include/kernqueue.h`](include/kernqueue.h); fields are native-endian for local use. Reserved fields must be zero. Zero-length requests are invalid. IDs are opaque and may be repeated by callers.

```text
C++ clients -> write -> per-open pending list -> kernel workqueue -> completed list -> read
                         |         mutex + wait queues                  |
                         +--------- bounded outstanding count ----------+
```

Completions follow submission order within one context. Concurrent writers are ordered by lock acquisition; concurrent readers compete for results. Separate opens cannot consume each other's results. The job itself runs outside the context mutex. User copies occur under a sleepable mutex on read; no spinlock is held across potentially faulting copies. Readiness is advisory and must be rechecked by callers after races. `O_NONBLOCK` prevents waiting for data/queue capacity; it does not make allocation, page faults or mutex contention non-sleeping.

Closing the last reference waits for an executing worker and frees all remaining jobs. There is no persistent state or cancel-job operation. See [design notes](docs/DESIGN.md) and [measured results](docs/RESULTS.md).

## Scope and limits

Tested kernel versions and actual scenarios are recorded with the evidence. This is not a production driver or a GPU scheduler. No hardware interrupts, MMIO, DMA, mmap, ioctl control plane, priority scheduling, memory pinning or cross-platform support are claimed. Queue limits are per open, not a global quota; many opens or blocked writers can still consume resources. `0600` access and use in a VM are deliberate. It has not been qualified with KASAN, KCSAN, lockdep, memory-leak instrumentation or a 32-bit userspace client.

The checksum is too small to benefit from kernel offload; this project measures interface behavior and correctness, not superiority over userspace checksumming.

## References

- [Linux miscellaneous devices](https://docs.kernel.org/driver-api/misc_devices.html)
- [Linux workqueues](https://docs.kernel.org/core-api/workqueue.html)
- [Linux wait queues and kernel APIs](https://docs.kernel.org/core-api/basics.html)
- [Linux VFS file operations](https://docs.kernel.org/filesystems/vfs.html)

License: GPL-2.0-only, matching the kernel-module source headers.
