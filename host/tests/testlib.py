"""Helpers the host suites' Python drivers share (host/tests/test_*.py import them from beside themselves)."""
import math

BASE91 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&()*+,-./:;<=>?@[]^_`{|}~"


def guid_of(queue_id):
    """genanki's guid_for: base91 of a 64-bit number, here the queue ID's first 64 bits."""
    v, out = int(queue_id[:16], 16), ""
    while True:
        out = BASE91[v % 91] + out
        v //= 91
        if not v:
            return out


def psnr(a, b):
    """PSNR (dB) of two RGB images of one size."""
    pa, pb = a.tobytes(), b.tobytes()
    err = sum((x - y) * (x - y) for x, y in zip(pa, pb)) / len(pa)
    return 99.0 if not err else 10 * math.log10(255 * 255 / err)
