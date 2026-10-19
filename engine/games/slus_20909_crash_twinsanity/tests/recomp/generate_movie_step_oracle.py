"""Compile unchanged NEW Movie player bodies and the live WaitFrame registration."""
import argparse
import hashlib
import json
from pathlib import Path
from recomp_paths import resolve  # the generated recomp TUs (PS2R_RECOMP_DIR)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    inventory = json.loads((args.root / "docs/design/movie_caller_inventory.json").read_text())
    step = next(row for row in inventory["bodies"] if row["entry"] == "0x002aea08")
    assert step["direct_callers"] == [] and step["literal_address_words"] == ["0x00306904"]
    start = next(row for row in inventory["bodies"] if row["entry"] == "0x002ae318")
    assert start["direct_callers"] == [] and start["literal_address_words"] == ["0x003068ec"]
    bodies, records = [], []
    for address, shard in [(0x2AEA08, 29), (0x2AE318, 29), (0x2B0DC8, 29),
                           (0x2B0FE0, 29), (0x2AF9B8, 29), (0x2B1630, 29), (0x2C6CE0, 30),
                           (0x2AE658, 29), (0x2AFA00, 29), (0x2AF568, 29)]:
        path = f"games/slus_20909_crash_twinsanity/recomp/recompiled_shard_{shard:03}.cpp"
        text = resolve(args.root, path).read_text()
        start = text.index(f"void FUN_{address:08x}(")
        end = text.index("\n// Function:", start)
        body = text[start:end]
        digest = hashlib.sha256(body.encode()).hexdigest()
        if address != 0x2C6CE0:
            # The caller inventory excludes trailing whitespace; compiled
            # provenance below retains the exact original body bytes.
            row = next(row for row in inventory["bodies"] if row["entry"] == f"0x{address:08x}")
            assert hashlib.sha256(body.rstrip().encode()).hexdigest() == row["body_sha256"], \
                "Movie player source changed: review caller/ABI inventory"
        bodies.append(body)
        records.append({"entry": f"0x{address:08x}", "source": path,
                        "source_sha256": digest, "compiled_sha256": digest, "omitted": ""})
    wait = inventory["wait_frame"]["live_registration"]
    text = resolve(args.root, wait["source"]).read_text()   # the generated common TU
    begin = text.index("    recompiled_functions[0x2af8f0] = [](CpuContext& ctx, uint32_t addr) {")
    end = text.index("\n    };", begin) + len("\n    };")
    registration = text[begin:end]
    digest = hashlib.sha256(registration.encode()).hexdigest()
    assert digest == wait["body_sha256"], "Review changed live WaitFrame registration"
    prefix = '''#include "games/slus_20909_crash_twinsanity/tests/recomp/movie_decoder_oracle_memory.h"
#include "engine/ee/cpu/ps2_float.h"
#include <map>
namespace movie_oracle {
void recomp_call(uint32_t, uint32_t address, CpuContext& c) { Dispatch(address,c); }
std::map<uint32_t,std::function<void(CpuContext&,uint32_t)>> recompiled_functions{
    {0x2b0fe0,[](CpuContext& c,uint32_t at) { Dispatch(at,c); }},
    {0x19fcb8,[](CpuContext& c,uint32_t at) { Dispatch(at,c); }}
};
'''
    bodies.append("void InstallWaitFrame() {\n" + registration + "\n}\n")
    dispatch = ["bool RunGenerated(uint32_t address,CpuContext& context) { switch (address) {"]
    for row in records:
        address = int(row["entry"], 16)
        dispatch.append(f"case 0x{address:x}: ObserveGenerated(address); "
                        f"FUN_{address:08x}(context,address); return true;")
    dispatch += ["case 0x2af8f0: { ObserveGenerated(address);",
                 "static const bool installed = [] { InstallWaitFrame(); return true; }(); (void)installed;",
                 "recompiled_functions.at(address)(context,address); return true; }",
                 "default: return false; } }", "}"]
    records.append({"entry": "0x002af8f0", "source": wait["source"],
                    "source_sha256": digest, "compiled_sha256": digest, "omitted": "",
                    "kind": "complete unchanged live registration lambda"})
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "movie_step_generated.cpp").write_text(prefix + "\n".join(bodies + dispatch) + "\n")
    (args.out / "movie_step_generated.json").write_text(json.dumps(records, indent=2) + "\n")
    print("Movie player: 10 complete NEW bodies + unchanged live WaitFrame wrapper; source/vtable pins matched")


if __name__ == "__main__":
    main()
