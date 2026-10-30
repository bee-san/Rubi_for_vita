#!/usr/bin/env python3
"""Check recount compiler frames plus an estimated helper/firmware allowance.

The three enclosing frames are measured by the compiler. The allowance is an estimate;
firmware I/O and formatting still need console stack high-water QA.
"""
import re
import sys
from pathlib import Path

build = Path(sys.argv[1])
frames = {}
for report in build.rglob("*.su"):
    for row in report.read_text().splitlines():
        fields = row.split("\t")
        if len(fields) != 3:
            continue
        for name in ("net_main", "stats_job", "vjo_history_total_step"):
            if re.search(r":" + name + r"(?:\.[\w.]+)?$", fields[0]):
                if fields[2] not in ("static", "dynamic,bounded"):
                    raise SystemExit(f"stats stack: unbounded frame: {row}")
                frames[name] = max(frames.get(name, 0), int(fields[1]))
if len(frames) != 3:
    raise SystemExit(f"stats stack: missing compiler frames: {frames}")
budget = Path(__file__).resolve().parents[1] / "include/vjo_ledger_budget.h"
limit = int(re.search(r"#define VJO_STACK_SHELL_NET (\d+)u", budget.read_text())[1])
allowance = 2048  # estimate for nested helpers/firmware, not a measured firmware maximum
used = sum(frames.values()) + allowance
print(f"stats stack estimate: {frames} + {allowance} B helper/firmware allowance = {used}/{limit} B")
if used > limit:
    raise SystemExit("stats stack: recount exceeds existing stack allowance")
