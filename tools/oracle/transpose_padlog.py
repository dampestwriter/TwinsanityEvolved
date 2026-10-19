#!/usr/bin/env python3
"""transpose_padlog.py PORT.padlog OFFSET END [--game-time ...] > STEM.oracle.padlog -- a port pad log (BOUNDARY BUTTONS LX LY RX
RY, each sample held to the next) as the oracle's `N-M:TOKEN` / `N-M:LX=v` ranges (engine/core/triage/input_log.cpp), every
boundary shifted by OFFSET (the oracle's boot movies play: START at 1350 for the port's 570 -> 780) and the log ending at END.
Buttons are one range per port sample so diff.py's k-th-press anchors line up; axes are merged runs of one value.

    python3 tools/oracle/transpose_padlog.py fixtures/padlogs/hub_entry.padlog 780 4200   # == the hand-written .oracle.padlog

GAME-TIME transposition. A constant boundary offset holds for the menus but not for play: the two games do not
advance their clocks alike per boundary. The port's TimeClockAdvanceBy yields 150 clock units (1/9000 s) a frame, retail's
and the oracle's EE arithmetic (round toward zero) 149 -- the port's game time runs 150/149 faster -- and the oracle's game
sees a 1174-unit frame TWICE every 512 boundaries (its timer 0 is tick-paced: the count reads exactly 0 at the game's
GameTimeTick once per 512 boundaries, the retail Ticks() overflow heuristic double-counts the wrap, the next frame keeps the
length: an oracle bug). From --port-play on, a port boundary is mapped to the first oracle frame whose
game time has reached the port's:

    T_port(b)   = port_units * (b - port_play)
    T_oracle(f) = oracle_units * (f - oracle_play) + (leap_units - oracle_units) * |{leap frames <= f}|

    python3 tools/oracle/transpose_padlog.py fixtures/padlogs/jungle_bungle_walk.padlog 780 7700 \\
        --game-time --port-play 1919 --oracle-play 2699 --leaps 2798,2799,3310,3311,3822,3823

The menu presses before --port-play keep the constant offset. The leap frames are measured on the oracle (oracle_probe.py
over harness.save_at_frame states: the clock word's +1024 per leap frame), 2798 + 512 k for the Jungle Bungle prefix until a
load stall re-phases the timer; --leap-period extends the list.
Note: the +780 transposed jungle_bungle_walk walk does NOT reproduce on the oracle -- its Crash lags the port's along the
beach from the first metres and leaves the path; the game-time map is the toolchain's answer."""
import argparse
import bisect
import sys

TOK = ["SELECT","L3","R3","START","UP","RIGHT","DOWN","LEFT","L2","R2","L1","R1","TRIANGLE","CIRCLE","CROSS","SQUARE"]
AX = ["LX","LY","RX","RY"]


def parse(src):
    samples = []
    for line in open(src):
        line = line.split("#")[0].strip()
        if not line:
            continue
        p = line.split()
        samples.append((int(p[0]), int(p[1], 16), [int(x) for x in p[2:6]]))
    samples.sort()
    return samples


class GameTimeMap:
    """port boundary -> oracle frame at equal game time (see the module doc)."""

    def __init__(self, offset, port_play, oracle_play, port_units, oracle_units, leap_units, leaps):
        self.offset, self.port_play, self.oracle_play = offset, port_play, oracle_play
        self.pu, self.ou, self.lu = port_units, oracle_units, leap_units
        self.leaps = sorted(set(leaps))

    def oracle_time(self, f):
        return self.ou * (f - self.oracle_play) + (self.lu - self.ou) * bisect.bisect_right(self.leaps, f)

    def __call__(self, b):
        if b < self.port_play:
            return b + self.offset
        t = self.pu * (b - self.port_play)
        # the first oracle frame whose game time (after its own update) has reached t: oracle_time is monotone
        lo, hi = self.oracle_play, self.oracle_play + 2 * (b - self.port_play) + 64
        while lo < hi:
            mid = (lo + hi) // 2
            if self.oracle_time(mid) >= t:
                hi = mid
            else:
                lo = mid + 1
        return lo


def transpose(samples, end, fmap):
    """The oracle lines: buttons one range per port sample (the k-th press anchors), axes as merged runs."""
    lines = []
    last = fmap(end)
    for i, (b, btn, ax) in enumerate(samples):
        nxt = samples[i + 1][0] if i + 1 < len(samples) else end + 1
        a, z = fmap(b), min(fmap(nxt) - 1, last)
        if z < a:
            z = a   # a press shorter than one oracle frame under the map still presses for one frame
        for bit, tok in enumerate(TOK):
            if (btn >> bit) & 1:
                lines.append((a, "%d-%d:%s" % (a, z, tok)))
    for i, tok in enumerate(AX):
        cur = None
        start = None
        for j, (b, btn, ax) in enumerate(samples):
            v = ax[i]
            if cur is not None and v != cur:
                lines.append((fmap(start), "%d-%d:%s=%d" % (fmap(start), fmap(b) - 1, tok, cur)))
                cur = None
            if cur is None and v != 128:
                cur, start = v, b
        if cur is not None:
            lines.append((fmap(start), "%d-%d:%s=%d" % (fmap(start), last, tok, cur)))
    lines.sort(key=lambda s: s[0])
    return [s for _, s in lines]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("offset", type=int)
    ap.add_argument("end", type=int)
    ap.add_argument("--game-time", action="store_true", help="map play boundaries by game time (see the module doc)")
    ap.add_argument("--port-play", type=int, default=2629, help="the port boundary play starts at (the menus before it keep the offset; 1919 before the drive-model re-recording)")
    ap.add_argument("--oracle-play", type=int, default=2699, help="the oracle frame play starts at")
    ap.add_argument("--port-units", type=int, default=149, help="the port's clock units a frame (TimeClockAdvanceBy under the EE rounding, the default; 150 before the flip)")
    ap.add_argument("--oracle-units", type=int, default=149, help="the oracle's clock units a frame (round toward zero)")
    ap.add_argument("--leap-units", type=int, default=1174, help="the oracle's clock units in a leap frame (1024 more)")
    ap.add_argument("--leaps", default="", help="the oracle frames whose game clock leaps, comma separated")
    ap.add_argument("--leap-period", type=int, default=512, help="extend the leap pairs by this period up to END + offset (0: none)")
    a = ap.parse_args()
    samples = parse(a.src)
    leaps = [int(x) for x in a.leaps.split(",") if x]
    if a.game_time and leaps and a.leap_period:
        base = sorted(leaps)
        horizon = a.end + a.offset + a.end // 10
        k = 1
        while min(base) + k * a.leap_period <= horizon:
            leaps += [f + k * a.leap_period for f in base]
            k += 1
        leaps = sorted(set(leaps))
    if a.game_time:
        fmap = GameTimeMap(a.offset, a.port_play, a.oracle_play, a.port_units, a.oracle_units, a.leap_units, leaps)
    else:
        fmap = lambda b, off=a.offset: b + off
    lines = transpose(samples, a.end, fmap)
    print("# padlog v1 start_frame=1 clock=frame title=slus_20909_crash_twinsanity")
    if a.game_time:
        print("# %s transposed for the NTSC oracle: +%d boundaries before port boundary %d (boot movies playing on the oracle); from it, by GAME TIME:"
              % (a.src, a.offset, a.port_play))
        print("# port %d units a frame from %d vs oracle %d units from %d, oracle leap frames (%d units) at %s" % (
            a.port_units, a.port_play, a.oracle_units, a.oracle_play, a.leap_units, ",".join(str(x) for x in leaps) or "none"))
        print("# port %d -> oracle %d" % (a.end, fmap(a.end)))
    else:
        print("# %s transposed by +%d boundaries for the NTSC oracle (boot movies playing on the oracle); analog held per sample" % (a.src, a.offset))
    print("\n".join(lines))


if __name__ == "__main__":
    main()
