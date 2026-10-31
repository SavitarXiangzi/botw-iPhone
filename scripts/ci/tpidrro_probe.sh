#!/usr/bin/env bash
# Probe: what does an EL0 write to TPIDRRO_EL0 do on Apple arm64?
#   sigill  -> the write raises SIGILL
#   changed -> the write lands (register is writable at EL0)
#   ignored -> no signal, value unchanged
set -euo pipefail
expect="${1:?usage: tpidrro_probe.sh sigill|changed|ignored}"
uname -m
cat > /tmp/tpidrro.c <<'EOF'
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
static sigjmp_buf jb;
static void on_sigill(int sig) { (void)sig; siglongjmp(jb, 1); }
int main(void) {
    uint64_t before = 0, after = 0;
    const uint64_t want = 0x1122334455667788ull;
    int caught = 0;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(before));
    signal(SIGILL, on_sigill);
    if (sigsetjmp(jb, 1) == 0) {
        __asm__ volatile("msr tpidrro_el0, %0" :: "r"(want));
    } else {
        caught = 1;
    }
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(after));
    if (caught) puts("RESULT sigill");
    else if (after == want) puts("RESULT changed");
    else puts("RESULT ignored");
    return 0;
}
EOF
xcrun clang -O0 -o /tmp/tpidrro /tmp/tpidrro.c
out="$(/tmp/tpidrro)"
echo "$out"
case "$expect" in
  sigill)  echo "$out" | grep -q "RESULT sigill" ;;
  changed) echo "$out" | grep -q "RESULT changed" ;;
  ignored) echo "$out" | grep -q "RESULT ignored" ;;
esac
