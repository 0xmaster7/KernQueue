#!/usr/bin/env python3
import json
import pathlib
import re
import statistics
root=pathlib.Path(__file__).resolve().parents[1]
log=(root/'docs/evidence/vm-console.log').read_text()
raw=log.split('BENCHMARK_JSON_BEGIN',1)[1].split('BENCHMARK_JSON_END',1)[0].strip()
results=json.loads(raw)
assert len(results['runs'])==10
assert all(r['verified_jobs'] == r['clients']*5000 for r in results['runs'])
(root/'docs/evidence/benchmark.json').write_text(json.dumps(results,indent=2)+'\n')
tests=re.search(r'TEST_SUMMARY scenarios=(\d+) failed=0',log)
assert tests and 'MODULE_RELOAD_CYCLES=5' in log and 'KERNEL_DIAGNOSTICS_CLEAN' in log
lines=['# Measured results','',
'Captured in an ephemeral QEMU TCG guest on pancakes. Software emulation; these are not native hardware or GPU performance numbers. Four virtual CPUs, 512 MiB RAM, no network. See `evidence/environment.txt` for versions and artifact hashes.','',
f"- Functional scenarios: **{tests.group(1)} passed, 0 failed**.",
'- Shared-descriptor concurrency: 2,000 unique results verified across four threads; each seen exactly once.',
'- Outstanding-work close: 200 cycles, 32 submitted jobs per cycle.',
'- Module unload/reload: five additional successful cycles.',
'- No BUG/WARNING/Oops/KASAN/general-protection-fault messages observed in the guest log. This is not a sanitizer or race-detector qualification.','',
'## Throughput','',
'Each configuration has five measured runs after one excluded 1,000-job warm-up. Each client submits batches of up to 32 before draining. Jobs use deterministic 1–256-byte payloads; every result, length, ID and checksum is verified in userspace. Timing includes open/close, thread creation, syscall/copy costs and verification.','',
'| Clients | Verified jobs per run | Median jobs/s | Min–max jobs/s |','|---|---:|---:|---:|']
for clients in (1,4):
    runs=[r for r in results['runs'] if r['clients']==clients]
    rates=[r['jobs_per_second'] for r in runs]
    lines.append(f"| {clients} | {runs[0]['verified_jobs']:,} | {statistics.median(rates):,.0f} | {min(rates):,.0f}–{max(rates):,.0f} |")
lat=results['latency']
lines += ['', '## Round-trip latency','',
'One blocking client, one outstanding job; 100 excluded warm-ups and 1,000 measured samples. Latency spans write to read completion; result verification occurs after the timed interval. Nearest-rank empirical percentiles; no tail-latency guarantee.','',
f"- p50: {lat['p50_us']:.2f} µs; p95: {lat['p95_us']:.2f} µs; p99: {lat['p99_us']:.2f} µs; maximum: {lat['max_us']:.2f} µs.",'',
'Raw per-run data: `evidence/benchmark.json`. Full boot, test and module lifecycle transcript: `evidence/vm-console.log`.','']
(root/'docs/RESULTS.md').write_text('\n'.join(lines))
print('\n'.join(lines))
