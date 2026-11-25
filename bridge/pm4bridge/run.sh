#!/bin/bash
# usage: run.sh <test binary name> [timeout secs] [args...]
# Runs one GPU test under `timeout`, logs to logs/, and reports new kernel
# trouble. Modelled on ~/rocm-src/runtest.sh.
cd "$(dirname "$0")" || exit 2
T="$1"; TO="${2:-60}"; shift; shift
mkdir -p logs
L="logs/$T.log"
C(){ journalctl -k -b --no-pager | grep -ciE "ring .*timeout|preemption time out|Failed to evict|kfd.*(hang|reset)"; }
K(){ journalctl -k -b --no-pager | grep -v "UFW BLOCK"; }
B=$(C); N0=$(K | wc -l)
{ echo "== $(date -Is) $T $* (timeout ${TO}s) env: ${PM4_ENV:-}"; } >> "$L"
timeout -k 5 "$TO" "tests/$T" "$@" >> "$L" 2>&1; RC=$?
A=$(C)
echo "== $T $* rc=$RC (124=timeout) trouble-count before=$B after=$A log=$L" | tee -a "$L"
grep -E "RESULT|FAIL|error|GB/s|GFLOP|us/dispatch" "$L" | tail -${TAILN:-25}
NEW=$(K | tail -n +$((N0+1)))
if [ -n "$NEW" ]; then echo "-- new kernel lines:"; echo "$NEW" | tail -30; echo "$NEW" >> "$L"; fi
pgrep -l -x "$T" && echo "!! $T still alive"
# D state is normal for a moment (kworkers doing I/O); only a process that is
# still in D two seconds later counts as stuck.
D1=$(ps -eo stat,pid,comm | awk '$1 ~ /^D/ {print $2}')
if [ -n "$D1" ]; then
    sleep 2
    for p in $D1; do
        ps -o stat=,pid=,comm= -p "$p" 2>/dev/null | awk '$1 ~ /^D/ {print "!! process stuck in D state:", $0}'
    done
fi
exit $RC
