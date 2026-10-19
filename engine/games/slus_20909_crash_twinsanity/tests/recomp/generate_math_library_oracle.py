#!/usr/bin/env python3
"""Extract the current NEW math bodies; never read OLD, an ELF, or a capture."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import unittest
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


SOURCE = "games/slus_20909_crash_twinsanity/recomp/recompiled_shard_009.cpp"
ENTRIES = (
    ("RotateVector", 0x18E4B8, 0),
    ("TransformPoint", 0x18E4F0, 0),
    ("TransformVector", 0x18E530, 0),
    ("MultiplyMatrices", 0x18E5A0, 0),
    ("IdentityMatrix", 0x18E570, 1),
    ("MultiplyRotations", 0x18D558, 2),
    ("RotationMatrix", 0x18E620, 3),
    ("Transpose", 0x18E6B0, 4),
    ("TransposeInPlace", 0x18E6F8, 5),
    ("TransposeRotation", 0x18E740, 4),
    ("InvertRigid", 0x18E788, 3),
    ("InvertRigidInPlace", 0x18E808, 5),
    ("TrianglePlane", 0x18D0A0, 6),
)


def code_only(text):
    """Mask comments and literals without changing offsets or line numbers."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                         re.DOTALL)
    # Generated bodies use ordinary literals. Refuse unsupported lexical forms
    # instead of silently treating braces in a raw string as program structure.
    if re.search(r'\bR"', text):
        raise ValueError("raw string literal requires an extractor update")
    masked = pattern.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), text)
    if "/*" in masked or '"' in masked or "'" in masked:
        raise ValueError("unterminated comment or literal")
    return masked


def extract(text, address, interiors=None):
    masked = code_only(text)
    signature = re.compile(
        rf"\bvoid\s+FUN_{address:08x}\s*\(\s*CpuContext\s*&\s*ctx\s*,"
        r"\s*uint32_t\s+addr\s*\)\s*\{")
    matches = list(signature.finditer(masked))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one FUN_{address:08x}, found {len(matches)}")
    opening = matches[0].end() - 1
    depth = 1
    end = opening + 1
    while end < len(masked) and depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    if depth:
        raise ValueError(f"unbalanced body FUN_{address:08x}")
    body = text[opening + 1:end - 1]
    code = masked[opening + 1:end - 1]
    if interiors is None:
        interiors = (0x18D120, 0x18D124) if address == 0x18D0A0 else ()
    expected = [f"Label_{index:04d}" for index in range(len(interiors) + 1)]
    if re.findall(r"\b(Label_\w+)\s*:", code) != expected:
        raise ValueError(f"interior labels changed for FUN_{address:08x}; review its ABI")
    if not re.search(rf"Label_0000:\s*//\s*0x{address:x}\b", body):
        raise ValueError(f"entry label/address mismatch for FUN_{address:08x}")
    cases = [int(value, 16) for value in re.findall(r"\bcase\s+(0x[0-9a-f]+)\s*:", code)]
    if cases != list(interiors):
        raise ValueError(f"interior dispatch changed for FUN_{address:08x}")
    if re.search(r"\bFUN_[0-9a-f]+\s*\(", code):
        raise ValueError(f"unexpected generated callee in FUN_{address:08x}")
    return body


PREAMBLE = r'''// Generated from NEW by generate_math_library_oracle.py. Do not edit.
#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_oracle.h"
#include "engine/ee/recomp_abi/host_app/cpu_state.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {
using QuadWord = GPR_reg;
MathLibraryOracle::Memory* current_memory = nullptr;
std::ofstream g_logFile;
struct UnexpectedVu {
    void ResumeIfWaiting(CpuContext&) { std::abort(); }
    void Execute(CpuContext&, uint32_t) { std::abort(); }
};
UnexpectedVu g_vu0, g_vu1;
namespace memory {
template<class T> T read(uint32_t address) {
    T value;
    std::memcpy(&value, current_memory->Translate(address, sizeof(value)), sizeof(value));
    return value;
}
void read_quad(uint32_t address, QuadWord& value) {
    std::memcpy(&value, current_memory->Translate(address & ~15u, 16), 16);
}
void write_quad(uint32_t address, const QuadWord& value) {
    std::memcpy(current_memory->Translate(address & ~15u, 16), &value, 16);
}
}  // namespace memory
'''


def generate(root, output):
    data = resolve(root, SOURCE).read_bytes()
    text = data.decode("utf-8")
    parts = [PREAMBLE]
    for name, address, _ in ENTRIES:
        body = extract(text, address)
        parts.append(f"\nvoid Oracle{name}(CpuContext& ctx, uint32_t addr) {{\n{body}\n}}\n")
    parts.append("}  // namespace\n\n")
    parts.append("const std::array<MathLibraryOracle::Entry, MathLibraryOracle::kEntryCount>&\n"
                 "MathLibraryOracle::Entries() {\n"
                 "    static const std::array<Entry, kEntryCount> entries{{\n")
    parts.extend(f'        {{"{name}", 0x{address:08x}u, {layout}}},\n'
                 for name, address, layout in ENTRIES)
    parts.append("    }};\n    return entries;\n}\n")
    parts.append("const char* MathLibraryOracle::SourceDigest() {\n"
                 f"    return {json.dumps(hashlib.sha256(data).hexdigest())};\n}}\n")
    parts.append("void MathLibraryOracle::Invoke(uint32_t function, CpuContext& context, Memory& memory) {\n"
                 "    struct RestoreMemory {\n"
                 "        Memory* previous;\n"
                 "        ~RestoreMemory() { current_memory = previous; }\n"
                 "    } restore{current_memory};\n"
                 "    current_memory = &memory;\n"
                 "    switch (function) {\n")
    parts.extend(f"        case 0x{address:08x}u: Oracle{name}(context, function); return;\n"
                 for name, address, _ in ENTRIES)
    parts.append("        default: std::abort();\n    }\n}\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(parts), encoding="utf-8")
    print(f"Extracted {len(ENTRIES)} NEW bodies; sha256={hashlib.sha256(data).hexdigest()}")


class ExtractionTests(unittest.TestCase):
    BODY = ('\nLabel_0000: // 0x18e4b8\n'
            ' { const char* s = "} \\" {"; /* } */ // {\n return; }\n')
    FUNCTION = "void FUN_0018e4b8(CpuContext& ctx, uint32_t addr) {" + BODY + "}\n"

    def test_exact_body_nested_braces_and_literals(self):
        self.assertEqual(extract(self.FUNCTION, 0x18E4B8), self.BODY)

    def test_missing_duplicate_and_malformed_are_rejected(self):
        for source in ("", self.FUNCTION * 2, self.FUNCTION[:-2],
                       self.FUNCTION.replace("/* } */", "/* }"),
                       self.FUNCTION.replace('"} \\" {"', 'R"({})"')):
            with self.subTest(source=source), self.assertRaises(ValueError):
                extract(source, 0x18E4B8)

    def test_new_entry_and_callee_require_review(self):
        for extra in ("Label_0001: ;", "case 0x18e4bc: ;", "FUN_00123456(ctx, 0);"):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                extract(self.FUNCTION.replace(" return;", extra + " return;"), 0x18E4B8)

    def test_changed_arithmetic_is_extracted_without_a_golden(self):
        changed = self.FUNCTION.replace(" return;", " ctx.vuRegs.Q = 3.0f; return;")
        self.assertIn("ctx.vuRegs.Q = 3.0f;", extract(changed, 0x18E4B8))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args()
    if arguments.selftest:
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(ExtractionTests))
        return 0 if result.wasSuccessful() else 1
    if arguments.root is None or arguments.output is None:
        parser.error("--root and --output are required unless --selftest is used")
    try:
        generate(arguments.root, arguments.output)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"MathLibraryOracle generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
