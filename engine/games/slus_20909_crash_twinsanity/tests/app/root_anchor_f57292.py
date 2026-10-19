#!/usr/bin/env python3
"""RootAnchorF57292 -- the f57292 anchor (OLD save_states/ps2state_slot0_f57292.ps2st) runs to
--frames 600 and finalizes with park=none (BUG-072: before the recompiler's
delay-slot rule this leg stopped at 15 frames). Body: root_anchor_frames.py."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import root_anchor_frames  # noqa: E402

if __name__ == "__main__":
    sys.exit(root_anchor_frames.run("RootAnchorF57292", sys.argv[1:]))
