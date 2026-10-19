#!/usr/bin/env python3
"""Extract NEW trig callers; expected math uses NEW's production libm model."""

import argparse
import hashlib
from pathlib import Path
import re
import sys

from generate_math_library_oracle import code_only, extract
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


ENTRIES = (
    ("CosSin16", 9, 0x18B918, ()),
    ("SinCos16", 30, 0x2C68F0, ()),
    ("SinCos16Pair", 30, 0x2C6988, ()),
    ("RotationFromPitch", 9, 0x18D590, (0x18D59C, 0x18D5B8, 0x18D618)),
    ("RotationFromYaw", 9, 0x18D620, (0x18D62C, 0x18D648, 0x18D6A8)),
    ("RotationFromRoll", 9, 0x18D6B0, (0x18D6BC, 0x18D6D8, 0x18D738)),
    ("GetRotationXY", 8, 0x1862A8, (0x1862B4, 0x1862C0, 0x1862DC, 0x18633C, 0x186348, 0x1863AC, 0x18643C)),
    ("GetRotationXZ", 8, 0x186448, (0x186454, 0x186460, 0x18647C, 0x1864DC, 0x1864E8, 0x18654C, 0x1865D8)),
    ("GetRotationYZ", 8, 0x1865E0, (0x1865EC, 0x1865F8, 0x186614, 0x186674, 0x186680, 0x1866E4, 0x186774)),
    ("MatrixAboutX", 9, 0x18BAE8, (0x18BAFC, 0x18BB04, 0x18BB0C, 0x18BBB4)),
    ("MatrixAboutY", 9, 0x18BBC0, (0x18BBD4, 0x18BBDC, 0x18BBE4, 0x18BC8C)),
    ("MatrixRotationZ", 9, 0x18BC98, (0x18BCAC, 0x18BCB4, 0x18BCBC, 0x18BD64)),
    ("GetRotationXYZ", 8, 0x186780, (0x186794, 0x1867AC, 0x1867B4, 0x1867BC, 0x1867D4,
                                  0x1867DC, 0x1867E4, 0x1867F4, 0x1867FC, 0x1868F4)),
    ("TanOfAngle", 9, 0x18B968, (0x18B9BC, 0x18B9D4, 0x18B9E8, 0x18B9F4)),
    ("MatrixFromPitchYaw", 8, 0x182AD0, (0x182AE4, 0x182AEC, 0x182AF4, 0x182AFC,
                                       0x182BA4, 0x182BAC, 0x182C54, 0x182D30, 0x182D34)),
    ("MatrixFromPitchRoll", 8, 0x182D40, (0x182D54, 0x182D5C, 0x182D64, 0x182D6C,
                                        0x182E14, 0x182E1C, 0x182EC4, 0x182FA0, 0x182FA4)),
    ("MatrixFromYawRoll", 8, 0x182FB0, (0x182FC4, 0x182FCC, 0x182FD4, 0x182FDC,
                                      0x183084, 0x18308C, 0x183134, 0x183210, 0x183214)),
    ("MatrixFromAngles", 8, 0x183220, (0x183234, 0x183250, 0x183258, 0x183260, 0x183274,
                                     0x18327C, 0x183284, 0x183298, 0x1832A0, 0x1833C4)),
    ("TurnToward", 9, 0x188300, (0x188350, 0x188354, 0x1883B4, 0x1883C8, 0x1883F8,
                                0x188508, 0x18850C)),
    ("TurnBySineCosine", 9, 0x1881C0, (0x1881D8, 0x188208, 0x188228, 0x188234,
                                      0x188240, 0x188258, 0x1882F4)),
    ("TurnAboutAxis", 9, 0x18DE00, (0x18DE14, 0x18DE60, 0x18DE64)),
    ("RotationFromAxisSine", 9, 0x18D818, (0x18D820, 0x18D85C, 0x18D884, 0x18D890)),
    ("RotationAboutAxis", 9, 0x18D798, (0x18D808,)),
    ("MatrixAboutAxis", 8, 0x185058, (0x185070, 0x1850A0, 0x1850C0, 0x1850CC, 0x1850D8)),
    ("AxisAngleMatrix", 9, 0x18CEA8, (0x18CEBC, 0x18CEC4, 0x18CECC, 0x18CF18, 0x18CF1C)),
    ("InverseLength4", 1, 0x11C0B8, (0x11C0F8, 0x11C10C)),
    ("SlerpRotations", 8, 0x187100, (0x187164, 0x18716C, 0x187194, 0x1871AC, 0x18720C,
                                     0x187220, 0x187294, 0x1872D8, 0x18731C, 0x187380,
                                     0x187390, 0x1873B0, 0x1873C0, 0x1873E8, 0x18744C,
                                     0x187480, 0x18749C)),
    ("AngleFrom", 1, 0x11BFA0, (0x11BFAC, 0x11BFB4, 0x11BFBC, 0x11BFD0, 0x11BFE0, 0x11BFF4)),
    ("AngleOfPoint", 8, 0x182918, (0x182948, 0x182970, 0x1829D4, 0x182A5C, 0x182A70, 0x182ABC)),
    ("EulerAnglesOfMatrix", 8, 0x185180, (
        0x1851CC, 0x1851D0, 0x1851DC, 0x1851F4, 0x1851F8, 0x185204, 0x18521C,
        0x185220, 0x18522C, 0x185244, 0x185248, 0x185254, 0x18527C, 0x185280,
        0x18528C, 0x1852AC, 0x1852B0, 0x1852BC, 0x1852D8, 0x1852DC, 0x1852E8,
        0x1852F0, 0x185300, 0x185310, 0x185314, 0x185338, 0x1853A8, 0x1853AC,
        0x1853D4, 0x1853E4, 0x1853F0, 0x185400, 0x185408, 0x18541C, 0x18542C,
        0x185434, 0x185444, 0x185460, 0x185464)),
    ("SnapToYaw", 8, 0x1874B8, (0x1874E4, 0x187504, 0x187550, 0x18755C, 0x187578, 0x1875C0)),
)

PREAMBLE = r'''// Extracted from current NEW; no OLD, captured data or golden results.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_trig_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {
using QuadWord = GPR_reg;
MathLibraryTrigOracle::Memory* current_memory = nullptr;
const MathLibraryTrigOracle::Dispatch* current_dispatch = nullptr;
std::array<uint64_t, MathLibraryTrigOracle::kEntryCount> body_calls{};
MathLibraryTrigOracle::Model g_vu0;
MathLibraryTrigOracle::UnexpectedCall g_vu1;
std::ofstream g_logFile;
namespace memory {
template<class T> T read(uint32_t address) {
    T value;
    std::memcpy(&value, current_memory->Translate(address, sizeof(value)), sizeof(value));
    return value;
}
template<class T> void write(uint32_t address, T value) {
    std::memcpy(current_memory->Translate(address, sizeof(value)), &value, sizeof(value));
}
void read_quad(uint32_t address, QuadWord& value) {
    std::memcpy(&value, current_memory->Translate(address & ~15u, 16), 16);
}
void write_quad(uint32_t address, const QuadWord& value) {
    std::memcpy(current_memory->Translate(address & ~15u, 16), &value, 16);
}
}
'''


def generate(root, output):
    sources = {}
    digest = hashlib.sha256()
    for shard in (1, 8, 9, 30):
        path = f"games/slus_20909_crash_twinsanity/recomp/recompiled_shard_{shard:03d}.cpp"
        data = resolve(root, path).read_bytes()
        sources[shard] = data.decode("utf-8")
        digest.update(path.encode() + b"\0" + data)
    for path in ("engine/vu/vu0/lib_vu0_sin_cos.cpp", "engine/vu/vu0/lib_vu0_sin_cos.h",
                 "engine/vu/vu_float.inl", "engine/vu/vu_chop_round_guard.cpp",
                 "engine/ee/cpu/ps2_float.h", "engine/ee/cpu/ps2_float.inl"):
        digest.update(path.encode() + b"\0" + resolve(root, path).read_bytes())
    parts = [PREAMBLE]
    parts.append("void recomp_call(uint32_t id, uint32_t address, CpuContext& context);\n")
    # Zero-angle matrices call this exact NEW generated dependency. Never
    # substitute the replacement identity operation into the expected side.
    identity = extract(sources[9], 0x18E570, ())
    if re.search(r"\brecomp_call\s*\(|\bg_vu[01]\b", code_only(identity)):
        raise ValueError("identity closure changed")
    parts.append(f"\nvoid OracleIdentityMatrix(CpuContext& ctx, uint32_t addr) {{\n{identity}\n}}\n")
    # Retain the exact NEW macro dependency, including its unopened log stream.
    # Control 21 only writes I; any unexpected model entry aborts in the test.
    rotation = extract(sources[9], 0x18E620, ())
    rotation_code = code_only(rotation)
    if (re.findall(r"\bswitch\s*\(\s*(\d+)\s*\)", rotation_code) != ["21"] or
            re.search(r"\brecomp_call\b|\bg_vu0\.Execute\b", rotation_code)):
        raise ValueError("rotation-matrix dependency changed; review its closure")
    parts.append(f"\nvoid OracleRotationMatrix(CpuContext& ctx, uint32_t addr) {{\n{rotation}\n}}\n")
    for name, shard, address, interiors in ENTRIES:
        body = extract(sources[shard], address, interiors)
        code = code_only(body)
        sites = 3 if name in ("GetRotationXY", "GetRotationXZ", "GetRotationYZ") else 1
        if name in ("GetRotationXYZ", "MatrixFromAngles", "TurnToward", "SlerpRotations"):
            sites = 2
        elif name in ("TurnBySineCosine", "RotationFromAxisSine", "MatrixAboutAxis", "InverseLength4",
                      "AngleFrom", "AngleOfPoint", "EulerAnglesOfMatrix"):
            sites = 0
        elif name.startswith("MatrixFrom"):
            sites = 4
        if re.findall(r"\bg_vu0\.Execute\s*\(\s*ctx\s*,\s*(0x[0-9a-f]+)\s*\)", code) != ["0xf0"] * sites:
            raise ValueError(f"{name}: expected {sites} source sin/cos operation sites")
        calls = re.findall(r"\brecomp_call\s*\(([^)]*)\)", code)
        expected = ["2422u,0x18e570u,ctx"] if name.startswith("Matrix") else []
        if name == "GetRotationXYZ":
            expected = ["2265u,0x1865e0u,ctx", "2264u,0x186448u,ctx", "2263u,0x1862a8u,ctx"]
        elif name == "MatrixFromAngles":
            expected = ["2240u,0x182fb0u,ctx", "2239u,0x182d40u,ctx", "2238u,0x182ad0u,ctx"]
        elif name == "TurnAboutAxis":
            expected = ["2277u,0x1881c0u,ctx"]
        elif name == "RotationAboutAxis":
            expected = ["2395u,0x18d818u,ctx"]
        elif name == "MatrixAboutAxis":
            expected = []
        elif name == "AxisAngleMatrix":
            expected = ["2422u,0x18e570u,ctx", "2253u,0x185058u,ctx"]
        elif name == "SlerpRotations":
            expected = ["187u,0x11c0b8u,ctx"]
        elif name == "EulerAnglesOfMatrix":
            expected = ["2237u,0x182918u,ctx", "182u,0x11bfa0u,ctx",
                        "2237u,0x182918u,ctx", "2237u,0x182918u,ctx",
                        "2237u,0x182918u,ctx", "182u,0x11bfa0u,ctx",
                        "2237u,0x182918u,ctx", "182u,0x11bfa0u,ctx"]
        elif name == "SnapToYaw":
            expected = ["2424u,0x18e620u,ctx", "2254u,0x185180u,ctx", "182u,0x11bfa0u,ctx"]
        if ([re.sub(r"\s+", "", call) for call in calls] != expected or
                re.search(r"\bResumeIfWaiting\b|\bg_vu1\b", code)):
            raise ValueError(f"{name}: unexpected model dependency; review its closure")
        parts.append(f"\nvoid Oracle{name}(CpuContext& ctx, uint32_t addr) {{\n{body}\n}}\n")
    parts.append("void recomp_call(uint32_t id, uint32_t address, CpuContext& context) {\n"
                 "    if (id == 2422u && address == 0x18e570u) {\n"
                 "        OracleIdentityMatrix(context, address); return;\n    }\n"
                 "    if (id == 2424u && address == 0x18e620u) {\n"
                 "        OracleRotationMatrix(context, address); return;\n    }\n"
                 "    if (!((id == 2263u && address == 0x1862a8u) ||\n"
                 "          (id == 2264u && address == 0x186448u) ||\n"
                 "          (id == 2265u && address == 0x1865e0u) ||\n"
                 "          (id == 2238u && address == 0x182ad0u) ||\n"
                 "          (id == 2239u && address == 0x182d40u) ||\n"
                 "          (id == 2240u && address == 0x182fb0u) ||\n"
                 "          (id == 2277u && address == 0x1881c0u) ||\n"
                 "          (id == 2395u && address == 0x18d818u) ||\n"
                 "          (id == 2253u && address == 0x185058u) ||\n"
                 "          (id == 187u && address == 0x11c0b8u) ||\n"
                 "          (id == 182u && address == 0x11bfa0u) ||\n"
                 "          (id == 2237u && address == 0x182918u) ||\n"
                 "          (id == 2254u && address == 0x185180u))) std::abort();\n"
                 "    if (current_dispatch && *current_dispatch) (*current_dispatch)(address, context);\n"
                 "    else MathLibraryTrigOracle::Invoke(address, context, *current_memory);\n}\n")
    parts.append("}  // namespace\n")
    parts.append("const std::array<MathLibraryTrigOracle::Entry, MathLibraryTrigOracle::kEntryCount>&\n"
                 "MathLibraryTrigOracle::Entries() {\n"
                 "    static const std::array<Entry, kEntryCount> entries{{\n")
    parts.extend(f'        {{"{name}", 0x{address:08x}u}},\n' for name, _, address, _ in ENTRIES)
    parts.append("    }};\n    return entries;\n}\n")
    parts.append("const char* MathLibraryTrigOracle::SourceDigest() {\n"
                 f'    return "{digest.hexdigest()}";\n}}\n')
    parts.append("std::array<uint64_t, MathLibraryTrigOracle::kEntryCount>\n"
                 "MathLibraryTrigOracle::BodyCalls() { return body_calls; }\n")
    parts.append("void MathLibraryTrigOracle::Invoke(uint32_t address, CpuContext& context, Memory& memory,\n"
                 "                                  const Dispatch& dispatch) {\n"
                 "    auto* previous = current_memory;\n"
                 "    auto* previous_dispatch = current_dispatch;\n"
                 "    current_memory = &memory;\n"
                 "    current_dispatch = &dispatch;\n"
                 "    try {\n    switch (address) {\n")
    parts.extend(f"        case 0x{address:08x}u: ++body_calls[{index}]; Oracle{name}(context, address); break;\n"
                 for index, (name, _, address, _) in enumerate(ENTRIES))
    parts.append("        default: std::abort();\n    }\n"
                 "    } catch (...) {\n"
                 "        current_memory = previous; current_dispatch = previous_dispatch; throw;\n    }\n"
                 "    current_memory = previous;\n    current_dispatch = previous_dispatch;\n}\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(parts), encoding="utf-8")
    print(f"Extracted {len(ENTRIES)} NEW trig bodies; sha256={digest.hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        generate(arguments.root, arguments.output)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"Trig oracle generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
