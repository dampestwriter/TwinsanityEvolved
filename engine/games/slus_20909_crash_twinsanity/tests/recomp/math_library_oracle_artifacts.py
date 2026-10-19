#!/usr/bin/env python3
"""Losslessly decode and verify actual math oracle artifacts, with bounded memory."""

import argparse
from contextlib import ExitStack
import hashlib
import io
import json
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest


def require(condition, message):
    if not condition:
        raise ValueError(message)


def exact(stream, length):
    data = stream.read(length)
    require(len(data) == length, f"truncated {getattr(stream, 'name', 'stream')}")
    return data


def delta(stream, base):
    result = bytearray(base)
    count, = struct.unpack("<I", exact(stream, 4))
    require(count <= len(base), "too many byte ranges")
    end = 0
    for _ in range(count):
        offset, length = struct.unpack("<II", exact(stream, 8))
        require(length > 0 and offset >= end and offset + length <= len(base),
                "overlapping, empty or out-of-bounds byte range")
        end = offset + length
        result[offset:end] = exact(stream, length)
    return result


def header(stream, magic, metadata):
    require(exact(stream, 8) == magic, "invalid stream magic")
    context, memory = struct.unpack("<II", exact(stream, 8))
    require((context, memory) == (metadata["context_size"], metadata["memory_size"]),
            "image sizes differ from metadata")
    require(0 < context + memory <= 64 * 1024 * 1024, "unreasonable image size")
    return context + memory


def output(stream, record, image, rounding):
    number, mode = struct.unpack("<QI", exact(stream, 12))
    require(number == record, "output record does not match input")
    require(mode == rounding, "output changed caller rounding")
    return delta(stream, image)


class Counters:
    def __init__(self, stream):
        self.stream = stream
        require(stream.readline() == '{"schema":1,"records":[\n', "invalid counter JSON header")
        self.count = 0
        self.previous = {}
        self.totals = {}
        self.last_had_comma = False

    def consume(self, record, step):
        line = self.stream.readline(1024 * 1024).strip()
        require(line.startswith('{"record":'), "missing actual counter record")
        if self.count:
            require(self.last_had_comma, "missing counter JSON separator")
        self.last_had_comma = line.endswith(",")
        row = json.loads(line[:-1] if self.last_had_comma else line)
        require(row["record"] == record, "counter record does not match output")
        values = row["values"]
        require(isinstance(values, dict) and bool(values), "missing actual counter values")
        if step == 0:
            self.previous = {}
        changes = {}
        for name, value in values.items():
            require(type(value) is int and value >= self.previous.get(name, 0), "counter decreased")
            changes[name] = value - self.previous.get(name, 0)
            self.totals[name] = self.totals.get(name, 0) + changes[name]
        require(set(self.previous) <= set(values), "counter disappeared during an installation")
        self.previous = values
        self.count += 1
        return changes

    def finish(self):
        require(not self.last_had_comma, "trailing counter JSON comma")
        line = self.stream.readline(1024 * 1024)
        require(line.startswith('],"count":'), "missing counter completion")
        footer = json.loads("{" + line[2:])
        require(footer["count"] == self.count and footer["totals"] == self.totals,
                "counter totals differ from actual call deltas")
        require(not self.stream.read(1), "unexpected counter trailing data")
        return self.totals


class GateExport:
    """Write only actual installed selected/control arms, grouped by boundary."""
    def __init__(self, directory, metadata, stack):
        directory.mkdir(parents=True, exist_ok=False)
        self.directory, self.metadata, self.stack = directory, metadata, stack
        self.entries = {}

    def record(self, entry, arm, image, actual, changes, record):
        if entry not in self.entries:
            directory = self.directory / f"{entry:08x}"
            directory.mkdir()
            files = {}
            for selection in ("selected", "control"):
                files[selection] = {
                    kind: self.stack.enter_context((directory / f"{selection}.{kind}.bin").open("wb"))
                    for kind in ("inputs", "outputs")}
                stream = self.stack.enter_context((directory / f"{selection}.counters.json").open("w"))
                stream.write('{"schema":1,"counter_mode":"actual call deltas","records":[\n')
                files[selection].update(counters=stream, count=0, totals={})
            self.entries[entry] = files
        file = self.entries[entry][arm]
        file["inputs"].write(image)
        file["outputs"].write(actual)
        if file["count"]:
            file["counters"].write(",\n")
        file["counters"].write(json.dumps({"record": record, "values": changes}, separators=(",", ":")))
        file["count"] += 1
        for name, change in changes.items():
            file["totals"][name] = file["totals"].get(name, 0) + change

    def finish(self):
        entries = []
        for entry, files in self.entries.items():
            selected, control = files["selected"]["count"], files["control"]["count"]
            require(selected == control and selected > 0, "unpaired gate export")
            for file in files.values():
                file["inputs"].close()
                file["outputs"].close()
                footer = json.dumps({"count": file["count"], "totals": file["totals"]}, separators=(",", ":"))
                file["counters"].write("\n]," + footer[1:] + "\n")
                file["counters"].close()
            metadata = {
                "schema": 1, "oracle": self.metadata["oracle"], "entry": entry,
                "command": self.metadata["command"],
                "context_size": self.metadata["context_size"], "memory_size": self.metadata["memory_size"],
                "selected_records": selected, "control_records": control,
                "control": ("already accepted TransformPoint replacement"
                            if entry == self.metadata["accepted_control_entry"] else
                            "actual selection=false installed library invoking captured originals"),
                "accepted_control": entry == self.metadata["accepted_control_entry"],
                "new_retirement_candidate": entry != self.metadata["accepted_control_entry"],
                "image_encoding": "raw complete context followed by complete fixture memory",
            }
            (self.directory / f"{entry:08x}" / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
            entries.append(metadata)
        (self.directory / "complete.json").write_text(json.dumps({
            "schema": 1, "command": self.metadata["command"], "entries": entries,
            "decoded_source_verified": True,
            "receipts": "Parent supplies signed receipts and source/binary digests",
        }, indent=2) + "\n")


def verify(directory, decode=None, gate_export=None):
    names = ("metadata.json", "complete.json", "inputs.bin", "model.outputs.bin",
             "selected.outputs.bin", "control.outputs.bin", "model.counters.json",
             "selected.counters.json", "control.counters.json")
    for name in names:
        require(stat.S_ISREG((directory / name).lstat().st_mode), f"{name} is not a regular file")
    metadata = json.loads((directory / "metadata.json").read_text())
    complete = json.loads((directory / "complete.json").read_text())
    require(metadata["schema"] == complete["schema"] == 1 and complete["oracle_passed"] is True,
            "oracle did not complete successfully")
    require(metadata["encoding"] == "math-byte-ranges-v1", "unsupported image encoding")
    command = metadata["command"]
    require(len(command) == 3 and command[1] == "--artifacts", "missing exact artifact invocation")
    require(complete["records"] > 0, "empty evidence")
    fixtures = {}
    for name, length in complete.get("fixtures", {}).items():
        require(Path(name).name == name and name not in names, "invalid fixture filename")
        path = directory / name
        require(stat.S_ISREG(path.lstat().st_mode) and path.stat().st_size == length,
                "missing or incomplete actual fixture")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(65536), b""):
                digest.update(chunk)
        fixtures[name] = {"bytes": length, "sha256": digest.hexdigest()}
    if decode is not None:
        decode.mkdir(parents=True, exist_ok=False)
    with ExitStack() as stack:
        gate = GateExport(gate_export, metadata, stack) if gate_export is not None else None
        binary = {name: stack.enter_context((directory / name).open("rb")) for name in
                  ("inputs.bin", "model.outputs.bin", "selected.outputs.bin", "control.outputs.bin")}
        counters = {arm: Counters(stack.enter_context((directory / f"{arm}.counters.json").open()))
                    for arm in ("model", "selected", "control")}
        size = header(binary["inputs.bin"], b"MATHIN01", metadata)
        for arm in ("model", "selected", "control"):
            header(binary[f"{arm}.outputs.bin"], b"MATHOU01", metadata)
        raw = {}
        index = None
        if decode is not None:
            raw = {arm: stack.enter_context((decode / f"{arm}.raw").open("wb"))
                   for arm in ("inputs", "model", "selected", "control")}
            index = stack.enter_context((decode / "index.jsonl").open("w"))
        hashes = {arm: hashlib.sha256() for arm in ("inputs", "model", "selected", "control")}
        previous = bytes(size)
        cases = 0
        last_key = None
        selected_inputs = {}
        paired_calls = 0
        for record in range(complete["records"]):
            fields = struct.unpack("<QQ7I", exact(binary["inputs.bin"], 44))
            number, case, entry, dispatch, operation, trial, step, selected, rounding = fields
            require(number == record and selected in (0, 1), "invalid input record index/selection")
            if step == 0:
                require(case == cases, "nonsequential installation number")
                cases += 1
            else:
                require(last_key is not None and case == cases - 1 and step == last_key[0] + 1 and
                        (selected, operation, trial, rounding) == last_key[1:],
                        "broken multi-call installation sequence")
            last_key = (step, selected, operation, trial, rounding)
            image = delta(binary["inputs.bin"], previous)
            previous = image
            arm = "selected" if selected else "control"
            model = output(binary["model.outputs.bin"], record, image, rounding)
            actual = output(binary[f"{arm}.outputs.bin"], record, image, rounding)
            require(model == actual, f"record {record}: decoded model/{arm} bytes differ")
            changes = counters["model"].consume(record, step)
            producer = f"generated_body_{entry:08x}"
            require(changes.get(producer) == 1 and sum(changes.values()) == 1,
                    "NEW model body counter does not match executed boundary")
            actual_changes = counters[arm].consume(record, step)
            pair_key = (entry, dispatch, operation, trial, step, rounding)
            if selected:
                if step == 0:
                    require(not selected_inputs, "selected case lacks a control partner")
                selected_inputs[step] = (pair_key, image)
            else:
                require(selected_inputs.pop(step, None) == (pair_key, image),
                        "selected/control did not consume identical case bytes")
                paired_calls += 1
            if gate:
                gate.record(entry, arm, image, actual, actual_changes, record)
            for label, data in (("inputs", image), ("model", model), (arm, actual)):
                hashes[label].update(data)
                if raw:
                    raw[label].write(data)
            if index:
                index.write(json.dumps(dict(zip(
                    ("record", "case", "entry", "dispatch", "operation", "trial", "step", "selected", "rounding"),
                    fields)), separators=(",", ":")) + "\n")
        require(not selected_inputs and cases == complete["cases"], "incomplete paired cases")
        for stream in binary.values():
            require(not stream.read(1), "unexpected binary trailing data")
        totals = {arm: counter.finish() for arm, counter in counters.items()}
        for arm, counter in counters.items():
            require(counter.count == complete[f"{arm}_records"], f"{arm} count differs from completion")
        require(complete["model_records"] == complete["records"] and
                complete["selected_records"] == complete["control_records"] == paired_calls,
                "missing model/selection results")
        if gate:
            gate.finish()
    return {"oracle": metadata["oracle"], "records": complete["records"], "cases": cases,
            "paired_calls": paired_calls, "differences": 0, "command": command,
            "accepted_control_entry": metadata["accepted_control_entry"],
            "actual_fixtures": fixtures,
            "decoded_sha256": {arm: value.hexdigest() for arm, value in hashes.items()},
            "actual_counter_totals": totals}


class FormatTests(unittest.TestCase):
    def test_lossless_ranges_keep_all_other_bytes(self):
        base = bytes(range(256))
        encoded = struct.pack("<III", 2, 0, 3) + b"\xff\x00\x80"
        encoded += struct.pack("<II", 250, 6) + b"\0" * 6
        actual = delta(io.BytesIO(encoded), base)
        self.assertEqual(actual, b"\xff\x00\x80" + base[3:250] + b"\0" * 6)
        self.assertEqual(delta(io.BytesIO(struct.pack("<I", 0)), base), base)

    def test_corrupt_or_truncated_ranges_fail_closed(self):
        examples = (b"", struct.pack("<III", 1, 3, 2) + b"xx",
                    struct.pack("<III", 1, 0, 0),
                    struct.pack("<III", 1, 0, 2) + b"x",
                    struct.pack("<III", 2, 0, 2) + b"xx" + struct.pack("<II", 1, 1) + b"x")
        for data in examples:
            with self.subTest(data=data), self.assertRaises(ValueError):
                delta(io.BytesIO(data), bytes(4))

    def test_counter_json_roundtrip_and_real_call_deltas(self):
        text = ('{"schema":1,"records":[\n'
                '{"record":0,"values":{"body":1}},\n'
                '{"record":1,"values":{"body":3}},\n'
                '{"record":2,"values":{"body":1}}\n'
                '],"count":3,"totals":{"body":4}}\n')
        self.assertEqual(json.loads(text)["totals"], {"body": 4})
        counter = Counters(io.StringIO(text))
        self.assertEqual(counter.consume(0, 0), {"body": 1})
        self.assertEqual(counter.consume(1, 1), {"body": 2})
        self.assertEqual(counter.consume(2, 0), {"body": 1})
        self.assertEqual(counter.finish(), {"body": 4})
        for changed in (text.replace('"body":4', '"body":5'),
                        text.replace('"count":3', '"count":4')):
            counter = Counters(io.StringIO(changed))
            for record, step in ((0, 0), (1, 1), (2, 0)):
                counter.consume(record, step)
            with self.assertRaises(ValueError):
                counter.finish()

    def test_gate_export_keeps_actual_arms_separate(self):
        # Deliberately distinct serialization-only values expose accidental
        # arm substitution. This temporary format fixture is not gate evidence.
        metadata = {"oracle": "format-unit", "command": ["format-unit"],
                    "context_size": 2, "memory_size": 2, "accepted_control_entry": 1}
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "gate"
            with ExitStack() as stack:
                export = GateExport(directory, metadata, stack)
                for record, arm in enumerate(("selected", "control", "selected", "control")):
                    export.record(1, arm, (arm[:2] + "in").encode(), (arm[:2] + "ou").encode(),
                                  {"entered": record + 1}, record)
                export.finish()
            for arm, prefix, total in (("selected", b"se", 4), ("control", b"co", 6)):
                self.assertEqual((directory / "00000001" / f"{arm}.inputs.bin").read_bytes(),
                                 (prefix + b"in") * 2)
                self.assertEqual((directory / "00000001" / f"{arm}.outputs.bin").read_bytes(),
                                 (prefix + b"ou") * 2)
                counters = json.loads((directory / "00000001" / f"{arm}.counters.json").read_text())
                self.assertEqual(counters["totals"], {"entered": total})
                self.assertEqual(counters["count"], 2)
            result = json.loads((directory / "00000001" / "metadata.json").read_text())
            self.assertEqual(result["command"], metadata["command"])
            self.assertTrue(result["accepted_control"])
            self.assertFalse(result["new_retirement_candidate"])


def io_test(binary):
    """Exercise the real writer's failure paths; these are not gate receipts."""
    import resource
    import signal

    def small_files():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (8192, 8192))

    with tempfile.TemporaryDirectory(prefix="math-artifact-io-") as temporary:
        root = Path(temporary)
        existing = root / "existing"
        existing.mkdir()
        (existing / "prior-evidence").write_bytes(b"must remain untouched")
        link = root / "link"
        link.symlink_to(existing, target_is_directory=True)
        for args in (["--artifacts"], ["--artifacts", str(existing)], ["--artifacts", str(link)]):
            result = subprocess.run([str(binary), *args], capture_output=True, timeout=10)
            require(result.returncode != 0, "invalid/existing output accepted")
        require(list(existing.iterdir()) == [existing / "prior-evidence"] and
                (existing / "prior-evidence").read_bytes() == b"must remain untouched",
                "existing evidence was changed")
        failed = root / "write-failure"
        result = subprocess.run([str(binary), "--artifacts", str(failed)], capture_output=True,
                                timeout=30, preexec_fn=small_files)
        require(result.returncode != 0 and (failed / "inputs.bin").exists(),
                "actual artifact write failure was not exercised")
        require(not (failed / "complete.json").exists(), "write failure published successful evidence")
    print("artifact I/O: invalid argv, existing directory/symlink and real write failure rejected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?")
    parser.add_argument("--decode", type=Path, help="also emit complete raw images and their record index")
    parser.add_argument("--gate-export", type=Path,
                        help="emit distinct actual selected/control raw streams and counter deltas per operation")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--io-test", type=Path, help="test an actual oracle writer's failure paths (POSIX)")
    arguments = parser.parse_args()
    if arguments.selftest:
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(FormatTests))
        return 0 if result.wasSuccessful() else 1
    if arguments.directory is None and arguments.io_test is None:
        parser.error("directory is required")
    try:
        if arguments.io_test is not None:
            io_test(arguments.io_test)
        else:
            print(json.dumps(verify(arguments.directory, arguments.decode, arguments.gate_export), sort_keys=True))
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        print(f"math artifact verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
