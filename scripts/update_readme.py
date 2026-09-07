#!/usr/bin/env python3
"""Regenerate the benchmark block in README.md from BENCHMARK_RESULTS.md.

The README must never carry a number that is not in the committed results
file. This script rewrites everything between <!-- BENCH:START --> and
<!-- BENCH:END --> from the first "Submit latency" block it finds, and
exits non-zero if README and results disagree (use it as a CI check with
--check).

encoding="utf-8" is explicit on every read/write, not decorative:
pathlib.Path.read_text()/write_text() default to the platform's
"preferred" locale encoding when no encoding is given — UTF-8 on Linux
(where this script's CI check runs), but cp1252 on Windows. Both files
this script touches contain non-ASCII characters (em dashes, arrows) as
a matter of course in normal technical writing, and cp1252 cannot
represent all of them — confirmed directly: running this script without
an explicit encoding on Windows raised UnicodeDecodeError partway
through README.md, despite the exact same script working without issue
in this repo's own Linux CI.
"""
import re, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
readme = root / "README.md"
results = root / "BENCHMARK_RESULTS.md"

txt = results.read_text(encoding="utf-8")
m = re.search(r"Submit latency.*?p50\s*:\s*(\d+)\s*ns.*?p99\s*:\s*(\d+)\s*ns.*?p99\.9:\s*(\d+)\s*ns", txt, re.S)
if not m:
    sys.exit("could not find submit latency block in BENCHMARK_RESULTS.md")
p50, p99, p999 = m.groups()
env = re.search(r"\*\*Environment:\*\*\s*(.*?)\n", txt)
env = env.group(1).strip().rstrip(",(").rstrip() if env else "see BENCHMARK_RESULTS.md"

block = f"""<!-- BENCH:START -->
### Order flow replay — 500K synthetic events ({env})

| Metric | Value |
|--------|-------|
| Submit latency p50 | **{p50} ns** |
| Submit latency p99 | {p99} ns |
| Submit latency p99.9 | {p999} ns |
<!-- BENCH:END -->"""

r = readme.read_text(encoding="utf-8")
new = re.sub(r"<!-- BENCH:START -->.*?<!-- BENCH:END -->", block, r, flags=re.S)
if "--check" in sys.argv:
    sys.exit(0 if new == r else "README benchmark block is out of date; run scripts/update_readme.py")
readme.write_text(new, encoding="utf-8")
print(f"README updated: p50={p50} p99={p99} p99.9={p999}")
