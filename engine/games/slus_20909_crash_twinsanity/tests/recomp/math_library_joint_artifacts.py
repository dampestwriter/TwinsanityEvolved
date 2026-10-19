#!/usr/bin/env python3
"""Verify JointLibrary's complete raw evidence and explicitly scoped source observations."""
import argparse
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import stat
import struct
import sys

from math_library_oracle_artifacts import Counters, delta, exact, header, output, require
from generate_math_library_joint_oracle import IMAGE_SHA256

# The JointLibrary oracles share this evidence layout. Rotate observes the
# twenty caller-visible vectors; Transform observes VF0..8 (constant, four
# result rows and the parent rows loaded by the source's own scalar code);
# Animate observes VF0..1 (constant and published frame rotation) and records
# its three counted angle-decoder delegations per selected frame.
SCOPES = {
    "MathLibraryJointAdapterOracle": {
        "entry": 0x297B40, "vectors": list(range(9)) + list(range(20, 29)) + [30, 31],
        "owned": ("rotations",), "generated": "generated_body_rotate",
        "attribution": "Discarded interpolation vector/integer/control temporaries; complete raw output retained."},
    "MathLibraryJointTransformOracle": {
        "entry": 0x294300, "vectors": list(range(9)),
        "owned": ("transforms", "matrices_written", "done_returns"), "generated": "generated_body_transform",
        "attribution": "Discarded basis/composition vector operands, integer/control temporaries, ACC, Q and "
                       "flags; complete raw output retained."},
    "MathLibraryJointAnimateOracle": {
        "entry": 0x293EC0, "vectors": [0, 1],
        "owned": ("animations",), "optional": ("blended_animations",), "delegated": {"angle_reads": 3},
        "generated": "generated_body_animate",
        "attribution": "Discarded frame-blend/turn/interpolation program operands and scratch vectors (VF2 "
                       "included after the turn program), integer/control temporaries, ACC, Q and flags; "
                       "complete raw output retained."},
}


def expected_counters(scope, selected, delta):
    """Actual per-case census deltas: one owned operation or one captured body, nothing else."""
    owned = dict.fromkeys(scope["owned"], 0)
    if selected:
        owned[scope["owned"][0]] = 1
        if len(scope["owned"]) > 1:
            # Transform splits its owned call into exactly one written matrix or one done return.
            kinds = [name for name in scope["owned"][1:] if delta.get(name) == 1]
            require(len(kinds) == 1, "selected transform must record one written matrix or one done return")
            owned[kinds[0]] = 1
    expected = {name: 0 for name in delta}
    expected.update(owned)
    for name in scope.get("optional", ()):
        # Animate records whether the selected frame was blended with the pose.
        require(delta.get(name) in (0, 1), f"{name} must be a per-case flag")
        expected[name] = delta[name] if selected else 0
    for name, count in scope.get("delegated", {}).items():
        expected[name] = count if selected else 0
    expected[scope["generated"]] = 0 if selected else 1
    return expected


def verify(directory):
    names = ("metadata.json", "complete.json", "inputs.bin", "model.outputs.bin",
             "selected.outputs.bin", "control.outputs.bin", "model.counters.json",
             "selected.counters.json", "control.counters.json", "source/complete.json")
    for name in names:
        require(stat.S_ISREG((directory / name).lstat().st_mode), f"{name} is not a regular file")
    metadata = json.loads((directory / "metadata.json").read_text())
    complete = json.loads((directory / "complete.json").read_text())
    observed = json.loads((directory / "source/complete.json").read_text())
    require(metadata["oracle"] in SCOPES and
            metadata["encoding"] == "math-byte-ranges-v1" and metadata["accepted_control_entry"] == 0,
            "unreviewed joint oracle")
    scope = SCOPES[metadata["oracle"]]
    ENTRY, VECTORS = scope["entry"], scope["vectors"]
    require(complete["oracle_passed"] and complete["records"] > 0 and
            complete["schema"] == metadata["schema"] == observed["schema"] == 1,
            "incomplete joint evidence")
    require(observed["entry"] == ENTRY and observed["vectors"] == VECTORS and
            observed["memory_bytes"] == metadata["memory_size"], "unreviewed source observation scope")
    fixture = directory / "standard_program"
    require(complete["fixtures"] == {"standard_program": 4096} and
            stat.S_ISREG(fixture.lstat().st_mode) and
            hashlib.sha256(fixture.read_bytes()).hexdigest() == IMAGE_SHA256,
            "actual original program fixture identity changed")
    command = metadata["command"]
    require(len(command) == 3 and command[1] == "--artifacts", "missing exact artifact invocation")
    offsets = metadata["context_offsets"]
    spans = [(offsets["cpuRegs"], observed["cpu_bytes"]), (offsets["fpuRegs"], observed["fpu_bytes"])]
    spans += [(offsets["vuRegs"] + reg * 16, 16) for reg in VECTORS]
    spans += [(metadata["context_size"], metadata["memory_size"])]
    require(sum(length for _, length in spans) == observed["output_record_bytes"],
            "source output size differs from its declared raw fields")

    def project(image):
        return b"".join(image[start:start + length] for start, length in spans)

    with ExitStack() as stack:
        streams = {name: stack.enter_context((directory / name).open("rb")) for name in
                   ("inputs.bin", "model.outputs.bin", "selected.outputs.bin", "control.outputs.bin")}
        size = header(streams["inputs.bin"], b"MATHIN01", metadata)
        require(observed["input_record_bytes"] == size + 8, "source input record size mismatch")
        for arm in ("model", "selected", "control"):
            header(streams[f"{arm}.outputs.bin"], b"MATHOU01", metadata)
        counters = {arm: Counters(stack.enter_context((directory / f"{arm}.counters.json").open()))
                    for arm in ("model", "selected", "control")}
        source = {}
        hashes = {}
        for arm in ("selected", "control"):
            for kind in ("inputs", "outputs"):
                key = f"{arm}.{kind}"
                path = directory / "source" / f"{key}.raw"
                require(stat.S_ISREG(path.lstat().st_mode), f"{key} is not a regular file")
                source[key] = stack.enter_context(path.open("rb"))
                hashes[key] = hashlib.sha256()
        previous = bytes(size)
        pending = None
        pairs = 0
        attributed = 0
        for record in range(complete["records"]):
            fields = struct.unpack("<QQ7I", exact(streams["inputs.bin"], 44))
            number, case, entry, dispatch, operation, trial, step, selected, rounding = fields
            require(number == case == record and entry == ENTRY and operation == step == 0 and
                    dispatch in (0, ENTRY) and selected in (0, 1), "unreviewed joint case shape")
            image = delta(streams["inputs.bin"], previous)
            previous = image
            arm = "selected" if selected else "control"
            model = output(streams["model.outputs.bin"], record, image, rounding)
            actual = output(streams[f"{arm}.outputs.bin"], record, image, rounding)
            actual_source = project(actual)
            require(project(model) == actual_source, f"record {record}: source output mismatch")
            if not selected:
                require(model == actual, f"record {record}: captured control differs from direct original")
            elif model != actual:
                attributed += 1  # Differences outside the complete observed fields are retained raw.
            model_delta = counters["model"].consume(record, step)
            require(model_delta == {f"generated_body_{ENTRY:08x}": 1}, "direct original counter mismatch")
            actual_delta = counters[arm].consume(record, step)
            require(scope["generated"] in actual_delta and set(scope["owned"]) <= set(actual_delta) and
                    actual_delta == expected_counters(scope, selected, actual_delta),
                    "actual captured-body/owned counters do not match the selected operation")
            raw_input = exact(source[f"{arm}.inputs"], observed["input_record_bytes"])
            raw_output = exact(source[f"{arm}.outputs"], observed["output_record_bytes"])
            require(raw_input == struct.pack("<II", dispatch, rounding) + image and
                    raw_output == actual_source, "source files substituted or omitted actual arm bytes")
            for kind, value in (("inputs", raw_input), ("outputs", raw_output)):
                hashes[f"{arm}.{kind}"].update(value)
            key = (entry, dispatch, operation, trial, step, rounding)
            if selected:
                require(pending is None, "selected source case is missing its control partner")
                pending = (key, raw_input, raw_output)
            else:
                require(pending == (key, raw_input, raw_output), "actual source arm pair differs")
                pending = None
                pairs += 1
        require(pending is None and pairs == observed["records_per_arm"] and
                complete["cases"] == complete["records"] == pairs * 2, "incomplete paired observations")
        for stream in (*streams.values(), *source.values()):
            require(not stream.read(1), "unexpected trailing artifact data")
        totals = {arm: counter.finish() for arm, counter in counters.items()}
        for arm, counter in counters.items():
            require(counter.count == complete[f"{arm}_records"], f"{arm} record count mismatch")
        require(complete["model_records"] == pairs * 2 and
                complete["selected_records"] == complete["control_records"] == pairs,
                "incomplete actual arms")
    return {"oracle": metadata["oracle"], "command": command, "pairs": pairs, "source_differences": 0,
            "selected_full_context_differences": attributed,
            "attribution": scope["attribution"],
            "actual_counter_totals": totals,
            "source_sha256": {key: value.hexdigest() for key, value in hashes.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.directory), sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(f"Joint artifact verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
