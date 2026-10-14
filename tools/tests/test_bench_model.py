"""tools/bench_stages.py and tools/bench_model.py: the source constants the job model quotes are the
source's, every workload has a recorded output checksum, and the report renders from a minimal input."""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import bench_model
import bench_stages

# Where each constant of bench_model.SOURCE lives, and the pattern that holds its value.
WHERE = {
    "worker_take_poll_ms": ("game_ocr/main.c", r"sceKernelDelayThread\(claim_outstanding \? \d+ : (\d+)\)", 1000),
    "shell_result_poll_ms": ("shell/game_ocr_client.c", r"delay_us\(client->ud, (\d+)\)", 1000),
    "control_poll_ms": ("shell/worker.c", r"#define POLL_TIMEOUT_US\s+(\d+)", 1000),
    "kernel_tick_ms": ("kernel/main.c", r"#define POLL_US\s+(\d+)", 1000),
    "signature_every_frames": ("kernel/vjo_kernel.h", r"#define VJO_CHECK_EVERY_FRAMES (\d+)", 1),
    "settle_ms": ("kernel/scene.h", r"#define SCENE_STABLE_US\s+(\d+)", 1000),
}


class BenchModelTest(unittest.TestCase):
    def test_source_constants_are_the_sources(self):
        self.assertEqual(set(WHERE), set(bench_model.SOURCE))
        for key, (path, pattern, scale) in WHERE.items():
            m = re.search(pattern, (ROOT / path).read_text())
            self.assertIsNotNone(m, f"{key}: {pattern} not in {path}")
            self.assertEqual(int(m.group(1)) // scale, bench_model.SOURCE[key][0], f"{key} in {path}")

    def test_every_workload_has_a_recorded_checksum(self):
        checks = json.loads(bench_stages.EXPECTED.read_text())["checks"]
        self.assertEqual({w["name"] for w in bench_stages.WORKLOADS}, set(checks))
        for name, value in checks.items():
            self.assertRegex(value, r"^[0-9a-f]{16}$", name)

    def test_report_renders(self):
        host = {"workloads": {"config": {"ops": 1, "ms_per_op": 0.0123, "counters": {"bytes": 4702, "warnings": 0}}}}
        report, md = bench_model.build_report(host, None, None, bench_stages.WORKLOADS)
        self.assertEqual([r["workload"] for r in report["stages"]], ["config"])
        self.assertIn("| `config` | parse | 12.3 µs | – | – | – | – | – |", md)


if __name__ == "__main__":
    unittest.main()
