"""Prepare and check the material extension against the actual mesh owner.

Writes only the specified build directory and optional owned patch artifact.
Never edits common mesh/root files. Exact snippets must match
require review, not fuzzy patching. The resulting complete mesh source is used
by the isolated consumer test until the common owner merges the extension.
"""
import argparse
import difflib
from pathlib import Path

DIRECTORY = "games/slus_20909_crash_twinsanity/hle/graphics"


def change(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f"M8 source changed; review extension site: {before[:80]}")
    return text.replace(before, after)


def header(text):
    text = change(text, "    struct Request { uint32_t material; MeshMaterial::Output shader; };",
        """    using Geometry = std::function<MeshGeometry::Result(
        const MeshGeometry::Mesh&, const std::vector<MeshGeometry::View>&,
        const ps2r::gs::DrawIr&, std::vector<ps2r::gs::DrawIr>&)>;
    struct Request {
        uint32_t material;
        MeshMaterial::Output shader;
        Geometry geometry{};
    };""")
    text = change(text, "        std::function<bool(uint32_t material)> owns_material;",
        """        std::function<bool(uint32_t material)> owns_material;
        // Optional complete material owner. Captures at the source flush,
        // with an earlier eligibility check during collection. Geometry
        // must own all deferred inputs and must not reread source memory.
        // False/throw refuses selection; after collection it faults the pass.
        std::function<bool(uint32_t, const MaterialState::Environment&,
                           MeshMaterial::Output&, Geometry&)> capture_material;""")
    return change(text, "    void Deliver(Material& material);",
        """    bool CaptureMaterial(uint32_t, const MaterialState::Environment&, Request&);
    void Deliver(Material& material);""")


def source(text):
    text = change(text, "bool MeshPass::Reserve(const std::vector<uint32_t>& addresses) {",
        """bool MeshPass::CaptureMaterial(uint32_t address, const MaterialState::Environment& environment,
                               Request& output) {
    Request request{address, {}};
    if (hooks_.capture_material) {
        if (!hooks_.capture_material(address, environment, request.shader, request.geometry) ||
            !request.geometry) return false;
    } else {
        if (MeshMaterial::Capture(hooks_.read, address, environment, hooks_.texture, request.shader) !=
            MeshMaterial::Result::Ok) return false;
        request.geometry = MeshGeometry::Build;
    }
    output = std::move(request);
    return true;
}
bool MeshPass::Reserve(const std::vector<uint32_t>& addresses) {""")
    text = change(text, """            if (MeshMaterial::Capture(hooks_.read, material, environment, hooks_.texture, request.shader) !=
                MeshMaterial::Result::Ok) return false;""",
        "            if (!CaptureMaterial(material, environment, request)) return false;")
    text = change(text, "            MeshMaterial::Output shader;", "            Request captured{address, {}};")
    text = change(text, """                MeshMaterial::Capture(hooks_.read, address, environment, hooks_.texture, shader) !=
                    MeshMaterial::Result::Ok) return false;""",
        "                !CaptureMaterial(address, environment, captured)) return false;")
    text = change(text, "        const auto& shader = material.requests[material.delivered].shader;",
        """        const auto& request = material.requests[material.delivered];
        const auto& shader = request.shader;""")
    return change(text,
        "            if (MeshGeometry::Build(part.block->model->parts[part.index], instances, shader.state, batch) !=",
        "            if (request.geometry(part.block->model->parts[part.index], instances, shader.state, batch) !=")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--patch", type=Path)
    args = parser.parse_args()
    patch = []
    for name, transform in (("mesh_pass.h", header), ("mesh_pass.cpp", source)):
        relative = f"{DIRECTORY}/{name}"
        before = (args.root / relative).read_text()
        after = transform(before)
        path = args.out / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(after)
        patch.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                         fromfile=f"a/{relative}", tofile=f"b/{relative}"))
    if args.patch:
        # Git accepts empty context lines without a leading space; this keeps
        # the patch artifact itself free of trailing whitespace.
        args.patch.write_text("".join(patch).replace("\n \n", "\n\n"))
    print("Prepared M8 source extension; original common files were read only.")


if __name__ == "__main__":
    main()
