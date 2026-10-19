"""Export the reviewed complete original archive's cloth material identities."""
import argparse
import json
from pathlib import Path

from audit_shadow_assets import HEADER_SHA, DATA_SHA


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.inventory.read_text())
    if (report["archive_header_sha256"] != HEADER_SHA or report["archive_data_sha256"] != DATA_SHA or
            report["material_ids"] != 50 or report["material_versions"] != 53 or
            report["draw_kinds"] != {"6": 424} or report["non_mesh_consumers"] or report["opaque_references"] or
            report["manifest_sha256"] != "0695f2c7cc15c718c32035ff209428c44fa0cd8552919311cad532e0bec32590"):
        raise ValueError("unreviewed cloth graph or geometry qualification")
    rows = report["material_records"] + report["other_versions"]
    if len(rows) != 53:
        raise ValueError("incomplete material versions")
    kinds = {1: "Ordinary", 0x17: "Single", 0x1a: "Three"}
    lines = ["// Generated from the complete verified archive by export_cloth_certificate.py.",
             "// Every consumer is a placed mesh; root still owns all-model and texture selection."]
    for row in sorted(rows, key=lambda r: (r["id"], r["sha256"])):
        record = row["source"]["record"]
        if len(record["shaders"]) != 1:
            raise ValueError("material pass graph changed")
        shader = record["shaders"][0]
        if shader["type"] not in kinds or not shader["texture"]:
            raise ValueError("cloth group material family changed")
        lines.append(f'    {{0x{row["id"]}u, 0x{shader["texture"]:08x}u, Kind::{kinds[shader["type"]]}, '
                     f'"{row["sha256"]}"}},')
    before, begin, tail = args.out.read_text().partition("// BEGIN GENERATED CLOTH RECORDS\n")
    _, end, after = tail.partition("// END GENERATED CLOTH RECORDS\n")
    if not begin or not end:
        raise ValueError("missing reviewed generated-record region")
    args.out.write_text(before + begin + "\n".join(lines) + "\n" + end + after)
    print("Exported 53 exact loaded-record identities for 50 cloth-family material IDs.")


if __name__ == "__main__":
    main()
