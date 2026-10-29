"""The hook-profile builder and its source inputs in synthetic installer checkouts."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]


def stage_profile_builder(checkout):
    """Use the real builder, including every file its content revision hashes."""
    revision = ROOT / "cmake/HookRevision.cmake"
    inputs = revision.read_text().split("set(inputs", 1)[1].split(")", 1)[0].split()
    for name in [*inputs, "cmake/HookRevision.cmake", "core/port.h"]:
        target = checkout / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, target)
