#!/usr/bin/env python3
"""Parallel, single-SSH-per-poll metrics collector for PS4 nodes."""

from __future__ import annotations

import configparser
import concurrent.futures
import json
import re
import subprocess
import threading
import time
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path


REMOTE_PROBE = r'''set +e
printf 'UP=1\n'
printf 'LOAD='; cut -d' ' -f1-3 /proc/loadavg
free -m | awk '/^Mem:/{print "MEM_TOTAL=" $2 "\nMEM_USED=" $3}'
if [ -r /sys/class/drm/card0/device/gpu_busy_percent ]; then
  printf 'GPU_BUSY='; cat /sys/class/drm/card0/device/gpu_busy_percent
elif command -v radeontop >/dev/null 2>&1; then
  printf 'GPU_BUSY='; radeontop -d - -l 1 2>/dev/null | awk 'match($0,/gpu [0-9.]+%/){v=substr($0,RSTART+4,RLENGTH-5); print int(v); exit}'
else echo 'GPU_BUSY='; fi
temps=''; max=''
for f in /sys/class/hwmon/*/temp*_input; do
  [ -r "$f" ] || continue; v=$(cat "$f" 2>/dev/null) || continue
  case "$v" in *[!0-9-]*|'') continue;; esac
  c=$(awk -v v="$v" 'BEGIN{printf "%.1f",v/1000}')
  label=$(cat "${f%_input}_label" 2>/dev/null); [ -n "$label" ] || label=$(basename "${f%_input}")
  temps="${temps}${temps:+,}${label}:${c}"
  max=$(awk -v a="${max:-$c}" -v b="$c" 'BEGIN{print (a>b?a:b)}')
done
printf 'TEMPS=%s\nTEMP_MAX=%s\n' "$temps" "$max"
for proc in llama-server rpc-server; do
  key=$(printf '%s' "$proc" | tr 'a-z-' 'A-Z_')
  if pgrep -x "$proc" >/dev/null 2>&1; then
    eval "printf '${key}=1\\n'"
  else eval "printf '${key}=0\\n'"; fi
done
pid=$(pgrep -xo llama-server 2>/dev/null)
model=''
if [ -n "$pid" ]; then
  cmd=$(tr '\000' '\n' < "/proc/$pid/cmdline" 2>/dev/null)
  model=$(printf '%s\n' "$cmd" | awk 'p{print;exit} $0=="-m"||$0=="--model"{p=1} /^--model=/{sub(/^--model=/,"");print;exit}')
fi
printf 'MODEL=%s\n' "$model"
sed -n '/^[A-Z][A-Z0-9_]*=/p' "$HOME/node-info.txt" 2>/dev/null | sed 's/^/INFO_/'
'''


@dataclass(frozen=True)
class Node:
    name: str
    ssh: str
    role: str
    rpc_port: int
    llama_server_port: int


def load_nodes(path: str | Path) -> dict[str, Node]:
    config = configparser.ConfigParser(interpolation=None)
    if not config.read(path):
        raise FileNotFoundError(path)
    nodes: dict[str, Node] = {}
    for name in config.sections():
        section = config[name]
        role = section.get("role", "worker")
        if role not in {"head", "worker"}:
            raise ValueError(f"{name}: role must be head or worker")
        nodes[name] = Node(name, section.get("ssh", name), role,
                           section.getint("rpc_port", 50052), section.getint("llama_server_port", 8080))
    if not nodes:
        raise ValueError("node inventory is empty")
    return nodes


def _number(value: str):
    try:
        return float(value) if "." in value else int(value)
    except (TypeError, ValueError):
        return None


def parse_probe(text: str) -> dict:
    raw = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            if re.fullmatch(r"[A-Z][A-Z0-9_]*", key):
                raw[key] = value
    info = {key[5:]: value for key, value in raw.items() if key.startswith("INFO_")}
    loads = [_number(x) for x in raw.get("LOAD", "").split()]
    temps = {}
    for item in raw.get("TEMPS", "").split(","):
        if ":" in item:
            label, value = item.rsplit(":", 1)
            temps[label] = _number(value)
    return {
        "up": raw.get("UP") == "1", "load": loads if len(loads) == 3 else [],
        "memory": {"used_mib": _number(raw.get("MEM_USED", "")), "total_mib": _number(raw.get("MEM_TOTAL", ""))},
        "gpu_busy": _number(raw.get("GPU_BUSY", "")), "temp_c": _number(raw.get("TEMP_MAX", "")),
        "temperatures": temps, "llama_server": raw.get("LLAMA_SERVER") == "1",
        "rpc_server": raw.get("RPC_SERVER") == "1", "model": raw.get("MODEL") or None, "info": info,
    }


class Collector:
    def __init__(self, nodes: dict[str, Node], interval: float = 5.0, timeout: float = 4.0, history_seconds: int = 3600):
        self.nodes, self.interval, self.timeout = nodes, interval, timeout
        self.history_len = max(2, int(history_seconds / interval))
        self._state: dict[str, dict] = {}
        self._history = defaultdict(lambda: defaultdict(lambda: deque(maxlen=self.history_len)))
        self._lock, self._stop = threading.Lock(), threading.Event()
        self._thread: threading.Thread | None = None

    def _poll_node(self, node: Node) -> tuple[str, dict]:
        now = time.time()
        try:
            result = subprocess.run(
                ["ssh", "-o", f"ConnectTimeout={max(1, int(self.timeout))}", "-o", "BatchMode=yes", "--", node.ssh, "sh", "-s"],
                input=REMOTE_PROBE, text=True, capture_output=True, timeout=self.timeout, check=False,
            )
            if result.returncode:
                raise RuntimeError(result.stderr.strip() or f"ssh exited {result.returncode}")
            data, error = parse_probe(result.stdout), None
        except (subprocess.TimeoutExpired, OSError, RuntimeError) as exc:
            data, error = ({"up": False, "load": [], "memory": {}, "gpu_busy": None, "temp_c": None,
                            "temperatures": {}, "llama_server": False, "rpc_server": False, "model": None, "info": {}}, str(exc))
        data.update({"name": node.name, "ssh": node.ssh, "role": node.role, "rpc_port": node.rpc_port,
                     "llama_server_port": node.llama_server_port, "timestamp": now, "error": error})
        return node.name, data

    def poll(self) -> None:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, len(self.nodes))) as pool:
            results = list(pool.map(self._poll_node, self.nodes.values()))
        with self._lock:
            for name, data in results:
                self._state[name] = data
                metrics = {"gpu": data.get("gpu_busy"), "mem": data.get("memory", {}).get("used_mib"),
                           "temp": data.get("temp_c"), "tok_s": data.get("tok_s")}
                for metric, value in metrics.items():
                    self._history[name][metric].append([data["timestamp"], value])

    def snapshot(self) -> dict:
        with self._lock:
            nodes = json.loads(json.dumps(self._state))
            history = {name: {metric: list(values) for metric, values in metrics.items()}
                       for name, metrics in self._history.items()}
        return {"generated_at": time.time(), "nodes": nodes, "history": history}

    def record_tok_s(self, name: str, value: float) -> None:
        """Add a benchmark result to the token-rate graph."""
        with self._lock:
            self._history[name]["tok_s"].append([time.time(), value])

    def run(self) -> None:
        while not self._stop.is_set():
            started = time.monotonic(); self.poll()
            self._stop.wait(max(0, self.interval - (time.monotonic() - started)))

    def start(self) -> None:
        if self._thread is None:
            self._thread = threading.Thread(target=self.run, name="collector", daemon=True); self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=self.timeout + 1)
