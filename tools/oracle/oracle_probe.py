#!/usr/bin/env python3
"""oracle_probe.py -- the port's --trace-player probe over the ORACLE's memory.

The oracle (the NTSC recomp) has no player probe; it can drop its 32 MB main memory per boundary (`--state-every K`
writes <leg>/memory_b<N>.bin + state_b<N>.ps2st) or one .ps2st (`--set harness.save_at_frame=N`). This reads the
played character's position / velocity / facing, the chunk camera and the game state out of either file through the
NTSC (SLUS-20909) retail globals -- the same walk runner/te_probe.cpp does over the PAL globals -- and prints the
probe's line so the two traces compare field for field.

NTSC globals (derived from the PAL symbol_addrs.txt by disassembling SwitchCharacter FUN_00177a90 in SLUS_209.09:
the four player words are stored in a row at 0x309574..0x309580, PAL 0x3098FC..0x309908 = -0x388; G_GameController
PAL 0x309888 -> 0x309500; the state word is read through the recomp's pad_ctrl_object_ptr 0x309508 = PAL 0x309890).

    oracle_probe.py FILE[.bin|.ps2st] [--boundary N]          one line
    oracle_probe.py --leg DIR [--delete]                      every memory_b*.bin in the leg, in order (--delete removes
                                                              each .bin and its .ps2st once read: the captures are 75 MB each)
    oracle_probe.py --watch DIR [--until N] [--delete]        poll DIR for new captures while the oracle runs
"""
import argparse
import os
import re
import struct
import sys
import time

G_PLAYER_CHARACTER = 0x00309574   # G_UnkPlayableCharObjInstCxt (PAL 0x003098FC)
# The game controller: the recomp's data_symbols.tsv pad_ctrl_object_ptr (PAL G_GameController_00309890 = 0x309890 - 0x388);
# the object it points at has the state word (case = bits 44..49) at +0 (the PAL probe reads G_GameController's +8:
# the two globals point 8 bytes apart into the same object)
G_GAME_CONTROLLER = 0x00309508
G_GLOBAL_CLOCK = 0x00323530       # G_GlobalClock_ (PAL 0x003238B0; the recomp's title_clock_word 0x323534 is its .time)
G_GAME_CLOCK_CONTROLLER = 0x0030972C   # G_GameClockController (PAL 0x00309ACC; found by its {frameTime 150, framesPerSecond 60} shape); GameTimeController::frameTime at +4
# Offsets as runner/te_probe.cpp (game/include/game/*.h CHECK_OFFSETs; the same object layout in both images)
AGENT_INSTANCE, CHARACTER_STATE, CREATURE_VELOCITY = 0x00, 0x70, 0x60
OBJECT_PLACE, OBJECT_CHUNK = 0x08, 0xA0
PLACE_POSITION, PLACE_MATRIX = 0x40, 0x00
CHUNK_MATRIX, CHUNK_PATH = 0x10, 0x138
RAM_SIZE = 32 << 20
STATE_NAMES = {0: "WaitingForLoader", 1: "Starting", 2: "WaitingForPad", 3: "CheckingCard", 4: "VivendiLogo", 5: "TravellersTalesLogo",
               6: "LoadingTitle", 7: "Title", 8: "MainMenu", 9: "NewGame", 10: "LoadingLevel", 11: "StartingPlay", 12: "Playing",
               13: "Watching", 14: "Movie", 15: "Paused", 16: "Dying", 17: "Waiting", 18: "RestartingTitle", 19: "Quitting",
               20: "Dead", 21: "FadingOut", 22: "Restarting", 23: "Loading", 24: "NoState"}


def ram_from_file(path):
    """The 32 MB RAM0 bytes of a .ps2st (PS2SAVE1 envelope) or a raw memory_b*.bin."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] == b"PS2SAVE1":
        version, count, vsync_frame, flags, wall = struct.unpack_from("<IIIIQ", data, 8)
        off = 32
        for _ in range(count):
            tag, sver, sflags, crc, plen = struct.unpack_from("<IIIIQ", data, off)
            off += 24
            if tag == struct.unpack("<I", b"RAM0")[0]:
                return data[off:off + plen], vsync_frame
            off += plen
        raise SystemExit("%s: no RAM0 section" % path)
    if len(data) != RAM_SIZE:
        raise SystemExit("%s: %d bytes, not a 32 MB memory image" % (path, len(data)))
    return data, None


class Ram:
    def __init__(self, data):
        self.d = data

    def ok(self, a, n=4):
        return a != 0 and a + n <= len(self.d)

    def u32(self, a):
        return struct.unpack_from("<I", self.d, a)[0] if self.ok(a) else 0

    def u64(self, a):
        return struct.unpack_from("<Q", self.d, a)[0] if self.ok(a, 8) else 0

    def f32(self, a):
        return struct.unpack_from("<f", self.d, a)[0] if self.ok(a) else 0.0

    def string(self, a):
        # game String: {chars*, length, capacity}
        p = self.u32(a)
        n = self.u32(a + 4)
        if not self.ok(p, 1) or n > 256:
            return ""
        return self.d[p:p + n].split(b"\0")[0].decode("latin-1")


def probe(ram):
    r = Ram(ram)
    s = {"valid": False}
    gc = r.u32(G_GAME_CONTROLLER)
    s["gstate"] = (r.u64(gc) >> 44) & 63 if r.ok(gc, 16) else None
    s["clock"] = r.u32(G_GLOBAL_CLOCK + 4)
    tc = r.u32(G_GAME_CLOCK_CONTROLLER)
    s["frame_time"] = r.u32(tc + 4) if r.ok(tc, 8) else None
    agent = r.u32(G_PLAYER_CHARACTER)
    if not r.ok(agent, 0x110):
        return s
    instance = r.u32(agent + AGENT_INSTANCE)
    if not r.ok(instance, 0x160):
        return s
    s["valid"] = True
    place = r.u32(instance + OBJECT_PLACE)
    chunk = r.u32(instance + OBJECT_CHUNK)
    s["chunk"] = r.string(chunk + CHUNK_PATH) if chunk else ""
    if r.ok(place, 0x70):
        bits = r.u64(place + 0x60)
        at = place + PLACE_MATRIX + 0x30 if bits & 4 else place + PLACE_POSITION
        s["pos"] = (r.f32(at), r.f32(at + 4), r.f32(at + 8))
        s["face"] = (r.f32(place + PLACE_MATRIX + 0x20), r.f32(place + PLACE_MATRIX + 0x28))
    state = r.u32(agent + CHARACTER_STATE)
    s["mode"], s["standing"], s["ground"], s["dead"] = (state >> 4) & 15, (state >> 8) & 15, (state >> 12) & 1, (state >> 14) & 1
    s["vel"] = (r.f32(agent + CREATURE_VELOCITY), r.f32(agent + CREATURE_VELOCITY + 4), r.f32(agent + CREATURE_VELOCITY + 8))
    if r.ok(chunk, 0x1F0):
        s["cam"] = (r.f32(chunk + CHUNK_MATRIX + 0x30), r.f32(chunk + CHUNK_MATRIX + 0x34), r.f32(chunk + CHUNK_MATRIX + 0x38))
        s["look"] = (r.f32(chunk + CHUNK_MATRIX + 0x20), r.f32(chunk + CHUNK_MATRIX + 0x28))
    return s


def line(boundary, s):
    gs = s.get("gstate")
    if not s["valid"]:
        return "[probe] @%s no player character (gstate %s) clock %u frame_time %s" % (boundary, gs, s.get("clock", 0), s.get("frame_time"))
    pos, vel, face = s.get("pos", (0, 0, 0)), s["vel"], s.get("face", (0, 0))
    cam, look = s.get("cam", (0, 0, 0)), s.get("look", (0, 0))
    return ("[probe] @%s pos (%.3f %.3f %.3f) vel (%.3f %.3f %.3f) face (%.2f %.2f) mode %u standing %u ground %u dead %u "
            "cam (%.1f %.1f %.1f) look (%.2f %.2f) gstate %s chunk %s clock %u frame_time %s" % (boundary, pos[0], pos[1], pos[2],
            vel[0], vel[1], vel[2], face[0], face[1], s["mode"], s["standing"], s["ground"], s["dead"], cam[0], cam[1], cam[2],
            look[0], look[1], gs, s.get("chunk") or "-", s.get("clock", 0), s.get("frame_time")))


def boundary_of(name):
    m = re.search(r"_b(\d+)\.", name)
    return int(m.group(1)) if m else None


def read_capture(path, delete):
    ram, vsync = ram_from_file(path)
    b = boundary_of(os.path.basename(path))
    if b is None:
        b = vsync
    out = line(b, probe(ram))
    if delete:
        os.remove(path)
    return b, out


def sweep_states(d, read, keep_after=0):
    """Delete the state_b<N>.ps2st / .refused of every boundary already read. The oracle writes memory_b<N>.bin FIRST and
    the .ps2st (77 MB) after it, so the state is not there yet when the .bin is read: it is swept on a later poll."""
    for n in os.listdir(d):
        m = re.match(r"^state_b(\d+)\.(ps2st|refused)$", n)
        if m and int(m.group(1)) in read and int(m.group(1)) >= keep_after:
            try:
                os.remove(os.path.join(d, n))
            except OSError:
                pass


def free_gb(path):
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize / float(1 << 30)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?")
    ap.add_argument("--boundary", type=int)
    ap.add_argument("--leg")
    ap.add_argument("--watch")
    ap.add_argument("--until", type=int, default=0, help="--watch: stop after the capture at this boundary (0: until the oracle exits)")
    ap.add_argument("--delete", action="store_true")
    a = ap.parse_args()
    if a.file:
        ram, vsync = ram_from_file(a.file)
        print(line(a.boundary if a.boundary is not None else vsync, probe(ram)))
        return
    d = a.leg or a.watch
    if not d:
        ap.error("FILE, --leg DIR or --watch DIR")
    seen = set()
    read = set()
    idle = 0
    min_free = float(os.environ.get("TE_MIN_FREE_GB", "3"))
    while True:
        names = sorted((boundary_of(n), n) for n in os.listdir(d) if re.match(r"^memory_b\d+\.bin$", n))
        fresh = [(b, n) for b, n in names if n not in seen]
        for b, n in fresh:
            # the writer renames .tmp -> final; a final file is complete
            seen.add(n)
            _, text = read_capture(os.path.join(d, n), a.delete)
            read.add(b)
            print(text, flush=True)
            if a.until and b >= a.until:
                if a.delete:
                    sweep_states(d, read)
                return
        if a.delete:
            sweep_states(d, read)
        if a.delete and free_gb(d) < min_free:
            # TE_MIN_FREE_GB: the captures are 75 MB each -- stop the oracle rather than fill the disk
            sys.stderr.write("[oracle_probe] free disk below %.0f GB: killing the oracle\n" % min_free)
            os.system("pkill -f ps2recomp_release")
        if not a.watch:
            return
        if fresh:
            idle = 0
        else:
            idle += 1
            if idle > 600:   # 5 minutes without a capture: the oracle is gone
                return
        time.sleep(0.5)


if __name__ == "__main__":
    main()
