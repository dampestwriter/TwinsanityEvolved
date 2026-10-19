#!/usr/bin/env python3
"""w2c_sigfix.py -- turn wasm-ld's `signature_mismatch:<fn>` stubs into type adapters (toolchain, not game code).

The decomp declares a few retail functions under two C++ signatures (`u16* CopyHalfword(u16*, const u16*)` in one TU,
`void CopyShort(u16*, const u16*)` in another, both RETAIL(MoveShortFromS2toS1)). wasm-ld warns "function signature
mismatch" and makes every DIRECT call of the minority signature go to a generated stub `signature_mismatch:<fn>` whose
body is `unreachable` -- a trap the first time the game takes that path (Cmd8_SpawnResidentAgent, after the first chunk
loads). The same rule as the vtable thunks and the call-site adapters applies: the caller's declared signature in, the
body's out. This pass rewrites each stub's body in the wasm2c output to call the real function: the arguments the stub
has are passed first (a shorter body takes the prefix; a longer one gets 0 for the rest), a missing result is 0, an
unused result is dropped. Idempotent; prints what it adapted so the build log records the count.

    w2c_sigfix.py build/w2c/game-impl.h build/w2c/game_*.c [--json report.json]
"""
import json
import re
import sys
from pathlib import Path

MARK = "/* te: signature adapter (tools/w2c_sigfix.py) */"
PROTO = re.compile(r"^(\w+) (w2c_\w+?)_signature_mismatch0x3A(\w+)\((w2c_\w+)\*((?:, \w+)*)\);$", re.M)


def proto_of(impl, module, name):
    m = re.search(rf"^(\w+) {re.escape(module)}_{re.escape(name)}\({re.escape(module)}\*((?:, \w+)*)\);$", impl, re.M)
    if not m:
        return None
    return m.group(1), [t for t in m.group(2).split(", ") if t]


def main():
    args = sys.argv[1:]
    report = None
    if "--json" in args:
        i = args.index("--json")
        report = args[i + 1]
        del args[i:i + 2]
    impl_path, shards = Path(args[0]), [Path(a) for a in args[1:]]
    impl = impl_path.read_text()
    stubs = []
    for m in PROTO.finditer(impl):
        ret, module, name, _inst, params = m.groups()
        stubs.append((ret, module, name, [t for t in params.split(", ") if t]))
    adapted, missing = [], []
    texts = {s: s.read_text() for s in shards}
    for ret, module, name, params in stubs:
        real = proto_of(impl, module, name)
        if real is None:
            missing.append(name)
            continue
        rret, rparams = real
        stub_fn = f"{module}_signature_mismatch0x3A{name}"
        args_c = []
        for i, t in enumerate(rparams):
            args_c.append(f"({t})var_p{i}" if i < len(params) else f"({t})0")
        call = f"{module}_{name}(instance{''.join(', ' + a for a in args_c)})"
        if ret == "void":
            body = f"{MARK} (void){call};"
        elif rret == "void":
            zero = "0.0f" if ret == "f32" else ("0.0" if ret == "f64" else "0")
            body = f"{MARK} {call}; return {zero};"
        else:
            body = f"{MARK} return ({ret}){call};"
        for s, text in texts.items():
            # the stub's definition: its prologue up to UNREACHABLE; replace that one line
            pat = re.compile(rf"(^{re.escape(ret)} {re.escape(stub_fn)}\({re.escape(module)}\* instance[^\n]*\{{\n(?:[^\n]*\n)*?)  UNREACHABLE;\n", re.M)
            new, n = pat.subn(lambda mm: mm.group(1) + "  " + body + "\n", text, count=1)
            if n:
                texts[s] = new
                adapted.append({"function": name, "stub": f"({', '.join(params)}) -> {ret}", "body": f"({', '.join(rparams)}) -> {rret}", "shard": s.name})
                break
        else:
            if not any(MARK in t and stub_fn in t for t in texts.values()):
                missing.append(name)
    for s, text in texts.items():
        if text != s.read_text():
            s.write_text(text)
    for a in adapted:
        print(f"w2c_sigfix: {a['function']}: callers of {a['stub']} now reach the body {a['body']} ({a['shard']})")
    print(f"w2c_sigfix: {len(stubs)} signature-mismatch stubs, {len(adapted)} adapted, {len(missing)} left as traps{': ' + ', '.join(missing) if missing else ''}")
    if report:
        Path(report).write_text(json.dumps({"stubs": len(stubs), "adapted": adapted, "missing": missing}, indent=1))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
