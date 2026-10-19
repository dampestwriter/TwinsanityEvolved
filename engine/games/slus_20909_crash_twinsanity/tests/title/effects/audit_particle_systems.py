"""Enumerate every particle system of the retail archive with its blend mode, without play.

The distortion (water / heat) route search: the title draws its
"hexagon" distortion blocks for a particle SYSTEM whose blend mode is BlendDistortion (7);
TR include/game/particles.h (ParticleBlendMode, ParticleDrawList), src/game/particles.cpp
ReadParticleSection / ReadParticleSystem (the per-version field order this reader mirrors)
and chunkfiles.h (the particles are section 8 of an SM2/RM2 root). The census names, per
container, every system and the blend-7 ones -- the route candidates. Never any payload copy.

Usage: --archive <dir with CRASH.BH/CRASH.BD> [--out report.json]
"""
import argparse
import collections
import json
import mmap
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "textures"))
from audit_texture_assets import archive_index, section  # noqa: E402

LEGACY = 0x20
OLDEST, NEWEST = 5, 0x1E
KEYS = 8
PARTICLE_SECTION = 8


class Reader:
    def __init__(self, data, at, end):
        self.data, self.at, self.end = data, at, end

    def bytes(self, n):
        if self.at + n > self.end:
            raise ValueError("particle section overrun")
        v = self.data[self.at:self.at + n]
        self.at += n
        return v

    def u32(self):
        return struct.unpack("<I", self.bytes(4))[0]

    def u16(self):
        return struct.unpack("<H", self.bytes(2))[0]

    def u8(self):
        return self.bytes(1)[0]

    def f32(self):
        return struct.unpack("<f", self.bytes(4))[0]


def read_system(r, version):
    """ReadParticleSystem's field order (TR particles.cpp:785-1080); returns the fields the census keeps."""
    name = bytes(r.bytes(16)).split(b"\0", 1)[0].decode("latin-1")
    if version == LEGACY:
        r.u8()
        r.u8()
    gen_rate = struct.unpack("<h", struct.pack("<H", r.u16()))[0]
    max_particles = r.u16()
    for _ in range(5):            # timingOffset, onTime(+random), offTime(+random)
        r.u16()
    gen_sort, gen_code, blend, _unused = r.u8(), r.u8(), r.u8(), r.u8()
    r.f32()                       # unusedFloat1
    if version == LEGACY and blend >= 0x80:
        blend -= 0x80             # LegacyBlendShift: the legacy files biased the mode
    if version >= 6:              # VersionCutRadiuses
        r.f32(); r.f32()
    if version >= 10:             # VersionDrawCutOff
        r.f32()
    if version >= 0x17 and version != LEGACY:
        r.f32()                   # unusedFloat5
    if version >= 0x18 and version != LEGACY:
        r.f32()                   # unusedFloat6
    if version < 7:
        r.u32(); r.u32()
    r.f32()                       # velocity
    r.bytes(12)                   # randomEmit
    if version < 0x12:
        r.bytes(12)
    r.bytes(12)                   # randomStart
    if version < 0x12:
        r.bytes(12)
    r.bytes(12 * 4)               # startRandomScale, startBase, velocityRandomScale, velocityBase
    r.f32(); r.f32()              # gravity, lifeTime
    r.u16(); r.u8(); r.u8(); r.f32()   # texture frames
    r.bytes(16)                   # jibber
    r.bytes(16 * KEYS)            # colourKeys
    r.bytes(8 * KEYS)             # alphaKeys
    if version >= 0x15:           # VersionDistortion
        r.f32(); r.f32()
    r.f32(); r.f32()              # min/max size
    r.bytes(8 * KEYS * 2)         # width, height keys
    r.f32(); r.f32()              # min/max rotation
    r.bytes(8 * KEYS * 3)         # rotation, unused1, unused2 keys
    r.bytes(16)                   # texture rectangle
    if version == LEGACY:
        r.u32()
    if version >= 3:              # VersionCollisionSpheres
        r.bytes(8 * KEYS)
        r.u8()
    draw_list = 0
    if version >= 0x11:           # VersionDrawList
        draw_list = r.u8()
    if version == LEGACY:
        r.u8(); r.u8(); r.u32()
    if version >= 0x17 and version != LEGACY:
        if version < 0x1D:        # VersionWithoutRecords
            for _ in range(r.u32()):
                r.bytes(16); r.u32(); r.u32()
    elif version == LEGACY:
        r.bytes(12 * 4)           # LegacyRecords
    if version >= 0x10:           # VersionGhosts
        if version == LEGACY:
            r.u16(); r.u8(); r.u8()
        else:
            r.u32()
        r.f32()
    if version >= 0x19 and version != LEGACY:
        r.u32(); r.f32()          # star
    if version >= 0x1A and version != LEGACY:
        r.f32()                   # rampTime
    page = None
    if version != LEGACY:
        if version >= 0x1B:
            page = r.u32()
        if version >= 0x1C:
            r.f32()               # scaleFactor
    if version >= 0x1E:
        r.bytes(16)               # boundingExtents
    return dict(name=name, gen_rate=gen_rate, max_particles=max_particles, gen_sort=gen_sort,
                blend=blend, draw_list=2 if blend == 7 else draw_list, texture_page=page)


def census(data, header):
    rows = []
    for name, start, size in archive_index(header):
        ext = Path(name).suffix.lower()
        if ext != ".rm2":             # the particles are the RM2's section 8 (the SM2 reads scenery only)
            continue
        root = section(data, start, size, 1)
        if PARTICLE_SECTION not in root:
            continue
        at, length = root[PARTICLE_SECTION]
        r = Reader(data, at, at + length)
        # The default chunk's section (ReadParticleData, defaultChunk): the three particle
        # pages' (textureId, materialId) words first (ReadParticlePageData fromStream=0),
        # then the systems, then the decal page/data behind them.
        default_chunk = name.lower().endswith("default.rm2")
        if default_chunk:
            for _ in range(3):
                r.u32(); r.u32()
        version = r.u32()
        if (version < OLDEST or version > NEWEST) and version != LEGACY:
            rows.append(dict(container=name, version=version, error="unreadable version"))
            continue
        count = r.u32()
        systems = [read_system(r, version) for _ in range(count)]
        rows.append(dict(container=name, version=version, default_chunk=default_chunk, systems=systems))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--archive", type=Path, required=True)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()
    header = (args.archive / "CRASH.BH").read_bytes()
    with (args.archive / "CRASH.BD").open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        rows = census(data, header)
    blends = collections.Counter()
    distortion = []
    for row in rows:
        for s in row.get("systems", []):
            blends[s["blend"]] += 1
            if s["blend"] == 7:
                distortion.append((row["container"], s["name"], s["gen_rate"], s["max_particles"]))
    print("containers with a particle section: %d; systems: %d" % (len(rows), sum(len(r.get("systems", [])) for r in rows)))
    print("blend modes:", dict(sorted(blends.items())))
    print("BlendDistortion (7) systems: %d" % len(distortion))
    by_container = collections.Counter(c for c, _, _, _ in distortion)
    for container, n in sorted(by_container.items(), key=lambda kv: -kv[1]):
        names = sorted({name for c, name, _, _ in distortion if c == container})
        print("  %3d %s  %s" % (n, container, ", ".join(names)))
    if args.out:
        args.out.write_text(json.dumps(dict(rows=rows, blend_modes={str(k): v for k, v in sorted(blends.items())},
                                            distortion=[dict(container=c, name=n, gen_rate=g, max_particles=m)
                                                        for c, n, g, m in distortion]), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
