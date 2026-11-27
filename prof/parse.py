#!/usr/bin/env python3
"""parse.py <tag> — summarize prof.sh output: wall ms/token vs GPU op time per token, top ops."""
import sys, csv, re, collections
tag = sys.argv[1]
rows = list(csv.DictReader(open(f"{tag}.plain.csv")))
tg = [r for r in rows if r.get("n_gen") not in (None, "0")]
ts = float(tg[0]["avg_ts"]); wall_us = 1e6 / ts
size = int(tg[0]["model_size"])
blocks, cur = [], None
for line in open(f"{tag}.perf.err", errors="ignore"):
    if line.startswith("Vulkan Timings:"):
        cur = {}; blocks.append(cur); continue
    m = re.match(r"(.+?): (\d+) x ([\d.e+]+) us = ([\d.e+]+) us", line)
    if m and cur is not None:
        cur[m.group(1)] = (int(m.group(2)), float(m.group(4)))
# tg blocks: single-token graphs; drop first 2 (warmup). Use the median block total.
tot = sorted(sum(v[1] for v in b.values()) for b in blocks)
if len(blocks) > 4: blocks = blocks[2:]
gpu_us = sorted(sum(v[1] for v in b.values()) for b in blocks)[len(blocks)//2]
agg = collections.defaultdict(lambda: [0, 0.0])
for b in blocks:
    for k, (n, t) in b.items():
        agg[k][0] += n; agg[k][1] += t
nb = len(blocks)
print(f"{tag}: {ts:.2f} tok/s -> wall {wall_us/1000:.2f} ms/token; model {size/2**30:.2f} GiB -> {size*ts/1e9:.1f} GB/s effective")
print(f"  GPU op time (median of {nb} graphs): {gpu_us/1000:.2f} ms/token = {100*gpu_us/wall_us:.0f}% of wall; overhead {(wall_us-gpu_us)/1000:.2f} ms")
nops = sum(v[0] for v in agg.values()) / nb
print(f"  dispatches/token ~{nops:.0f}; avg {gpu_us/nops:.1f} us each")
for k, (n, t) in sorted(agg.items(), key=lambda kv: -kv[1][1])[:12]:
    print(f"  {100*t/sum(v[1] for v in agg.values()):5.1f}%  {t/nb/1000:7.3f} ms/tok  {n/nb:5.0f}x  {k[:90]}")
