#!/usr/bin/env python3
"""Diag helper: keep only the named f<key>.ppm / p<key>.ppm frames (the target grab and the
presented image) in a --dump-frames directory while a leg runs; the per-draw f<key>_drawn_* dumps go."""
import os, re, sys, time
d, keep = sys.argv[1], set(int(k) for k in sys.argv[2:])
while True:
    try:
        for n in os.listdir(d):
            m = re.fullmatch(r"[fp](\d+)(_drawn_0x[0-9a-fA-F]+)?\.ppm", n)
            if m and (m.group(2) or int(m.group(1)) not in keep):
                try: os.remove(os.path.join(d, n))
                except OSError: pass
    except OSError: pass
    time.sleep(0.5)
