"""Inventory read-only TR renderer declarations and source program families."""
import argparse
import csv
import hashlib
import json
import re
import subprocess
from pathlib import Path


def comments_blanked(text):
    # Preserve strings and line numbers; parentheses in prose are not functions.
    tokens = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/'
    return re.sub(tokens, lambda m: re.sub(r"[^\n]", " ", m[0])
                  if m[0].startswith(("//", "/*")) else m[0], text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tr", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    mapping = args.root / "games/slus_20909_crash_twinsanity/symbols/pal_ntsc_map.tsv"
    with mapping.open() as stream:
        names = {row["name"]: row for row in csv.DictReader(
            (line for line in stream if not line.startswith("#")), delimiter="\t")}
    renderer = args.tr / "src/platform/ps2/renderer"
    paths = sorted(renderer.glob("*.cpp")) + [renderer / "renderer.h",
        args.tr / "src/game/shapes.cpp", args.tr / "src/game/overlay.cpp"]
    files, declarations, other_bindings = [], [], []
    annotation = r"RETAIL(?:_[A-Z0-9]+)?\(([^()]*)\)"
    for path in paths:
        text = path.read_text()
        clean = comments_blanked(text)
        relative = str(path.relative_to(args.tr))
        files.append(dict(path=relative, sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        functions = list(re.finditer(r"\b([A-Za-z_]\w*)\s*\([^;{}]*?\)\s*" + annotation, clean))
        function_annotations = set()
        for match in functions:
            retail = match[2]
            mapped = names.get(retail, {})
            function_annotations.add(clean.index("RETAIL", match.start(), match.end()))
            declarations.append(dict(name=match[1], source=relative,
                line=clean[:match.start()].count("\n") + 1, retail_symbol=retail,
                ntsc_candidate=mapped.get("ntsc_addr"), mapping_confidence=mapped.get("confidence"),
                status="source declaration; binding/caller closure required"))
        for match in re.finditer(annotation, clean):
            if match.start() not in function_annotations:
                other_bindings.append(dict(source=relative,
                    line=clean[:match.start()].count("\n") + 1, retail_symbol=match[1],
                    status="non-function binding; no function/caller closure claim"))
    programs = comments_blanked((renderer / "vuprograms.cpp").read_text())
    registrations, empty = [], []
    for match in re.finditer(r"void (Register\w+Program)\(\)\s*\{([^}]*)\}", programs):
        call = re.search(r"\bRegister\(([^;]*)\);", match[2])
        if not call:
            empty.append(dict(registration=match[1],
                              source_line=programs[:match.start()].count("\n") + 1))
            continue
        row = dict(registration=match[1], source_line=programs[:match.start()].count("\n") + 1,
                   operands=[part.strip() for part in call[1].split(",")],
                   status="open; source parameters plus NTSC image/entry arithmetic required")
        if match[1] == "RegisterShaderType0AProgram":
            row.update(status="Sky typed geometry/material/load conversion implemented; complete "
                "generated-boundary self-oracles pass. Root integration/live gate open. Other type-A "
                "callers are not closed.", ntsc=dict(registration="0x001dcca8", code="0x002e3b40",
                instructions=174, second_entry_instructions=83))
        registrations.append(row)
    result = dict(tr_commit=subprocess.check_output(
        ["git", "-C", str(args.tr), "rev-parse", "HEAD"], text=True).strip(),
        policy="Static source inventory, with comments excluded. Repeated declarations are retained. "
        "PAL/NTSC mappings are candidates, not hook proof. A program registration is not caller closure. "
        "No captured runtime image or external emulator is coverage evidence.",
        mapping_sha256=hashlib.sha256(mapping.read_bytes()).hexdigest(),
        files=files, declarations=declarations, non_function_bindings=other_bindings,
        vu_program_registrations=registrations, no_code_registrations=empty,
        renderer_resident_programs=dict(ids=[1, 2, 10, 13, 11],
            source="src/platform/ps2/renderer/vuprograms.cpp:293",
            status="Sky behavior covered through type-A geometry. Other users remain open; "
                   "these resident programs are not globally retired."),
        program_aliases=[dict(registration=row["registration"], code_owner=row["operands"][1],
            status="shared image does not imply identical material inputs or caller closure")
            for row in registrations if row["registration"] == "RegisterShaderType1BProgram"])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(f"{len(files)} source files, {len(declarations)} function declarations, "
          f"{len(other_bindings)} non-function bindings, {len(registrations)} program families")


if __name__ == "__main__":
    main()
