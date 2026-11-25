#!/usr/bin/env python3
"""Parsers and CLI used by tools/bench.sh for llama-bench output."""

from __future__ import annotations

import argparse
import json
import re
import sys


def _test_kind(value: str):
    match = re.search(r"\b(pp|tg)\s*(\d+)\b", value.lower())
    return (match.group(1), int(match.group(2))) if match else (None, None)


def _combine(rows):
    tests = {}
    for row in rows:
        key = (row.get("model", ""), row.get("ngl"))
        test = tests.setdefault(key, {"model_reported": row.get("model") or None, "ngl_reported": row.get("ngl"),
                                      "pp": None, "pp_tokens": None, "tg": None, "tg_tokens": None})
        kind = row["kind"]
        test[kind], test[f"{kind}_tokens"] = row["speed"], row["count"]
    return list(tests.values())


def parse_json_output(text: str) -> list[dict]:
    data = json.loads(text)
    if isinstance(data, dict):
        data = data.get("results", data.get("benchmarks", [data]))
    rows = []
    for item in data:
        n_prompt, n_gen = item.get("n_prompt", 0), item.get("n_gen", 0)
        if n_prompt:
            kind, count = "pp", int(n_prompt)
        elif n_gen:
            kind, count = "tg", int(n_gen)
        else:
            kind, count = _test_kind(str(item.get("test", "")))
        speed = item.get("avg_ts", item.get("tokens_per_second", item.get("t_s")))
        if kind and speed is not None:
            rows.append({"kind": kind, "count": count, "speed": float(speed),
                         "model": item.get("model_filename", item.get("model", "")),
                         "ngl": item.get("n_gpu_layers", item.get("ngl"))})
    return _combine(rows)


def parse_markdown_output(text: str) -> list[dict]:
    rows = []
    for line in text.splitlines():
        if "|" not in line or re.match(r"^\s*\|?\s*[-:]+", line):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        test_index = next((i for i, cell in enumerate(cells) if _test_kind(cell)[0]), None)
        if test_index is None:
            continue
        kind, count = _test_kind(cells[test_index])
        speed = None
        for cell in reversed(cells[test_index + 1:]):
            match = re.search(r"(-?\d+(?:\.\d+)?)\s*(?:±.*)?$", cell)
            if match:
                speed = float(match.group(1)); break
        if speed is None:
            continue
        ngl = next((int(cell) for cell in reversed(cells[:test_index]) if cell.isdigit()), None)
        rows.append({"kind": kind, "count": count, "speed": speed, "model": cells[0], "ngl": ngl})
    return _combine(rows)


def parse_output(text: str, output_format: str = "auto") -> list[dict]:
    if output_format in {"json", "auto"}:
        try:
            return parse_json_output(text)
        except (json.JSONDecodeError, TypeError, ValueError):
            if output_format == "json":
                raise
    return parse_markdown_output(text)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--format", choices=("auto", "json", "markdown"), default="auto")
    parser.add_argument("--meta", required=True, help="JSON object added to every parsed test")
    args = parser.parse_args()
    meta = json.loads(args.meta)
    for test in parse_output(sys.stdin.read(), args.format):
        print(json.dumps({**meta, **test}, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
