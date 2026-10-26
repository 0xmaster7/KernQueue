#!/usr/bin/env bash
# Build and execute only inside an ephemeral, networkless QEMU guest.
set -euo pipefail
cd "$(dirname "$0")/.."
KERNEL_VERSION=${KERNEL_VERSION:-$(uname -r)}
KERNEL_IMAGE=${KERNEL_IMAGE:-/boot/vmlinuz-$KERNEL_VERSION}
export KDIR=${KDIR:-/lib/modules/$KERNEL_VERSION/build}
make module
make static-client
mkdir -p build/root/{bin,dev,proc,sys,tmp} docs/evidence
cp /usr/bin/busybox build/root/bin/busybox
ln -sf busybox build/root/bin/sh
cp build/kqctl build/root/kqctl
cp kernel/kernqueue.ko build/root/kernqueue.ko
cat > build/root/init <<'INIT'
#!/bin/sh
B=/bin/busybox
$B mount -t proc proc /proc
$B mount -t sysfs sysfs /sys
$B mount -t devtmpfs devtmpfs /dev
fail() { echo KERNQUEUE_VM_FAIL; $B dmesg; $B poweroff -f; }
$B insmod /kernqueue.ko || fail
/kqctl checksum hello || fail
/kqctl test || fail
echo BENCHMARK_JSON_BEGIN
/kqctl bench || fail
echo BENCHMARK_JSON_END
$B rmmod kernqueue || fail
for n in 1 2 3 4 5; do
    $B insmod /kernqueue.ko || fail
    /kqctl checksum hello || fail
    $B rmmod kernqueue || fail
done
echo MODULE_RELOAD_CYCLES=5
# O/E taints are expected for an unsigned out-of-tree module. Kernel diagnostics are not.
if $B dmesg | $B grep -E 'BUG:|WARNING:|Oops:|KASAN:|general protection fault'; then fail; fi
echo KERNEL_DIAGNOSTICS_CLEAN
echo KERNQUEUE_VM_PASS
$B poweroff -f
INIT
chmod +x build/root/init
(cd build/root && find . -print0 | cpio --null -o --format=newc 2>/dev/null) | gzip -1 > build/initramfs.cpio.gz
if test -r "$KERNEL_IMAGE"; then cp "$KERNEL_IMAGE" build/vmlinuz; else sudo -n cp "$KERNEL_IMAGE" build/vmlinuz; sudo -n chown "$(id -u):$(id -g)" build/vmlinuz; fi
{
    date -u +%FT%TZ
    uname -a
    qemu-system-x86_64 --version | head -1
    gcc --version | head -1
    echo 'Guest: TCG software emulation, 4 vCPUs, 512 MiB RAM, network disabled'
    sha256sum kernel/kernqueue.ko build/kqctl build/vmlinuz kernel/kernqueue.c tools/kqctl.cpp include/kernqueue.h
} > docs/evidence/environment.txt
timeout 240 qemu-system-x86_64 -accel tcg,thread=multi -smp 4 -m 512 \
    -kernel build/vmlinuz -initrd build/initramfs.cpio.gz \
    -append 'console=ttyS0 rdinit=/init panic=-1 oops=panic quiet' \
    -nographic -monitor none -no-reboot -nic none 2>&1 | tee docs/evidence/vm-console.log
grep -q '^KERNQUEUE_VM_PASS' docs/evidence/vm-console.log
! grep -q '^KERNQUEUE_VM_FAIL' docs/evidence/vm-console.log
python3 scripts/summarize.py
