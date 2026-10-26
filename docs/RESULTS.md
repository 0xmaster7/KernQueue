# Measured results

Primary recorded guest kernel: **7.0.0-1017-azure**. The same functional suite also passed **6.17.0-1022-azure**; its complete evidence is archived in `evidence/kernel-6.17/`. No functional source edits were made between the two runs.

Captured in an ephemeral QEMU TCG guest on pancakes. Software emulation; these are not native hardware or GPU performance numbers. Four virtual CPUs, 512 MiB RAM, no network. See `evidence/environment.txt` for versions and artifact hashes.

- Functional scenarios: **14 passed, 0 failed**.
- Shared-descriptor concurrency: 2,000 unique results verified across four threads; each seen exactly once.
- Outstanding-work close: 200 cycles, 32 submitted jobs per cycle.
- Module unload/reload: five additional successful cycles.
- No BUG/WARNING/Oops/KASAN/general-protection-fault messages observed in the guest log. This is not a sanitizer or race-detector qualification.

## Throughput

Each configuration has five measured runs after one excluded 1,000-job warm-up. Each client submits batches of up to 32 before draining. Jobs use deterministic 1–256-byte payloads; every result, length, ID and checksum is verified in userspace. Timing includes open/close, thread creation, syscall/copy costs and verification.

| Clients | Verified jobs per run | Median jobs/s | Min–max jobs/s |
|---|---:|---:|---:|
| 1 | 5,000 | 57,585 | 28,992–61,438 |
| 4 | 20,000 | 44,129 | 42,539–50,976 |

## Round-trip latency

One blocking client, one outstanding job; 100 excluded warm-ups and 1,000 measured samples. Latency spans write to read completion; result verification occurs after the timed interval. Nearest-rank empirical percentiles; no tail-latency guarantee.

- p50: 178.46 µs; p95: 320.12 µs; p99: 400.18 µs; maximum: 923.27 µs.

Raw per-run data: `evidence/benchmark.json`. Full boot, test and module lifecycle transcript: `evidence/vm-console.log`.
