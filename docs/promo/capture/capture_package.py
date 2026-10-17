#!/usr/bin/env python3
"""capture_package.py: the release, with the promo's capture app in place of its Overlay app.

    capture_package.py CHECKOUT BASE.zip CAPTURE.self OUT_BASE.zip

CHECKOUT is the vita-jp-overlay checkout both were built from; BASE.zip its Vita build's base zip (cmake --build
--target release); CAPTURE.self the capture app's eboot (the smoke target with app_capture_patch.py's scripts).
OUT_BASE.zip is BASE.zip with its app VPK's eboot.bin replaced by CAPTURE.self and its stamp (vjo_app.txt, made by
the checkout's own tools/app_stamp.py) and the release's record of the app (ux0/data/VitaJPOverlay/vjo_app.txt) made
for that eboot: everything else, entry for entry and in order (param.sfo, the icon, the LiveArea art), is the
release's. Then the checkout's tools/package_release.py (--models) and tools/package_vpk.py make the one-VPK
release from it as they make the real one (capture/run.sh), so the Install page in Vita3K shows the plan, the
sizes and the strings of that release."""
import importlib.util
import io
import sys
import zipfile
from pathlib import Path

root, base, self_path, out = (Path(a) for a in sys.argv[1:5])
spec = importlib.util.spec_from_file_location("app_stamp", root / "tools" / "app_stamp.py")
app_stamp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(app_stamp)

eboot = self_path.read_bytes()
api = app_stamp.api_version(root / "include" / "vjo_api.h")
stamp = app_stamp.make(eboot, api)
stamp = stamp if isinstance(stamp, str) else stamp.decode("ascii")
names = set(zipfile.ZipFile(base).namelist())
app_name = next(n for n in ("Rubi.vpk", "JPOverlaySettings.vpk") if n in names)  # the app's VPK, before or after the rename
record = "ux0/data/VitaJPOverlay/vjo_app.txt"


def app_vpk(original):
    buf = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(original)) as src, zipfile.ZipFile(buf, "w") as dst:
        seen = set()
        for info in src.infolist():
            data = src.read(info)
            if info.filename == "eboot.bin":
                data = eboot
            elif info.filename == "vjo_app.txt":
                data = stamp.encode("ascii")
            seen.add(info.filename)
            dst.writestr(info, data)
        if not {"eboot.bin", "vjo_app.txt"} <= seen:
            sys.exit(f"capture_package.py: {app_name} has no eboot.bin or vjo_app.txt")
    return buf.getvalue()


with zipfile.ZipFile(base) as src, zipfile.ZipFile(out, "w") as dst:
    for info in src.infolist():
        data = src.read(info)
        if info.filename == app_name:
            data = app_vpk(data)
        elif info.filename == record:
            data = stamp.encode("ascii")
        dst.writestr(info, data)
print(f"{out}: {app_name} with the capture eboot ({len(eboot)} B), {stamp.strip()}")
