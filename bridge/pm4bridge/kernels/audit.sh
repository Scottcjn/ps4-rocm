#!/bin/bash
# usage: audit.sh k.s   - enforce the PS4 Pro GPU hardware rules on compiler output
s="$1"; rc=0
if grep -nE '^\s*flat_' "$s"; then echo "AUDIT FAIL $s: FLAT instruction (hangs the GPU)"; rc=1; fi
if grep -nE 'ds_(read|write)_b128' "$s"; then echo "AUDIT FAIL $s: ds b128 (wrong LDS data)"; rc=1; fi
if grep -nE 'amdhsa_private_segment_fixed_size' "$s" | grep -vE 'fixed_size 0$'; then
    echo "AUDIT FAIL $s: needs scratch"; rc=1; fi
if grep -nE 'ScratchSize: [1-9]' "$s"; then echo "AUDIT FAIL $s: scratch in use"; rc=1; fi
[ $rc = 0 ] && echo "audit ok: $s"
exit $rc
