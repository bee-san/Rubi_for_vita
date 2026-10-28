"""The pins and package lists that CI's workflow (.github/workflows/release.yml), the fast gate's images
(tools/gate/*.Dockerfile), tools/ci_container_build.sh and the docs each repeat are one: the gate builds with what
CI builds with (AGENTS.md "Merge gate"), and a pin bumped in one place but not another fails here, not as a gate
that quietly builds other bytes than CI. The vita-vn-ocr commits are tools/tests/test_vocr_defaults.py's
(RuntimePinEverywhere) and test_spot_pins.py's."""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FLOW = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def apt_install(text):
    """The packages of a Dockerfile's or script's `apt-get install -y --no-install-recommends ...` (one command,
    continued over lines)."""
    m = re.search(r"apt-get install -y --no-install-recommends ((?:\\\n|[^\n&\\])+)", text)
    return m.group(1).replace("\\\n", " ").split()


class VitaImage(unittest.TestCase):
    def test_ci_the_gate_and_the_container_build_run_one_vitasdk_image(self):
        digest = re.search(r"^    container: vitasdk/vitasdk@sha256:([0-9a-f]{64})$", FLOW, re.M).group(1)
        self.assertIn(f"\nFROM docker.io/vitasdk/vitasdk@sha256:{digest}\n", read("tools/gate/vita.Dockerfile"))
        self.assertIn(f"IMAGE=${{VJO_CI_IMAGE:-docker.io/vitasdk/vitasdk@sha256:{digest}}}\n",
                      read("tools/ci_container_build.sh"))
        self.assertIn(f"`vitasdk/vitasdk@sha256:{digest}`", read("docs/offline-testing.md"))
        # the vita job's two caches are keyed by it: another image never restores this one's packages or toolchain
        keys = re.findall(r"key: (apt-vita|vita-toolchain)-([0-9a-f]+)-", FLOW)
        self.assertEqual(sorted(keys), [("apt-vita", digest[:8])] * 2 + [("vita-toolchain", digest[:8])] * 2)

    def test_the_vita_job_installs_the_gate_images_packages(self):
        ci = re.search(r"^ +- run: bash tools/ci_apt_cache\.sh \.refs/apt-vita (.+)$", FLOW, re.M).group(1).split()
        self.assertEqual(sorted(apt_install(read("tools/gate/vita.Dockerfile"))), sorted(ci))
        self.assertEqual(sorted(apt_install(read("tools/ci_container_build.sh"))), sorted(ci))


class HostImage(unittest.TestCase):
    def test_the_gate_host_image_has_every_package_the_host_and_arm_jobs_install(self):
        lists = re.findall(r"tools/ci_apt_cache\.sh \.refs/apt-(host|arm) (.+)$", FLOW, re.M)
        self.assertEqual(sorted(job for job, _ in lists), ["arm", "host"])
        image = set(apt_install(read("tools/gate/host.Dockerfile")))
        for job, packages in lists:
            for package in packages.split():
                with self.subTest(job=job, package=package):
                    self.assertIn(package, image)


class Dictionary(unittest.TestCase):
    def test_ci_checks_out_the_pinned_dictionary_and_the_docs_name_it(self):
        """docs/plaintext-dictionaries.json's revision, which tools/fast_gate.sh and tools/prepare_plaintext.py read,
        is the one the host, ARM and Vita jobs check out, and the one the guide and docs/local-dictionaries.md name."""
        revision = json.loads(read("docs/plaintext-dictionaries.json"))["revision"]
        refs = re.findall(r"repository: bee-san/plaintext-dicts\n(?: +#.*\n)* +ref: ([0-9a-f]{40})\n", FLOW)
        self.assertEqual(refs, [revision] * 3)
        for doc in ("docs/guide.md", "docs/local-dictionaries.md"):
            with self.subTest(doc=doc):
                self.assertIn(f"commit `{revision[:7]}`", read(doc))


if __name__ == "__main__":
    unittest.main()
