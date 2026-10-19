"""recomp_paths -- where the title's GENERATED recompiler output lives.

The recompiled EE code (recompiled.h, recompiled.cpp, recompiled_shard_NNN.cpp, continuations.h)
is derived from the game's ELF and is NOT committed: the build regenerates it into the build
directory (cmake/TitleInputs.cmake, PS2R_RECOMP_GEN_DIR) and runs every script that reads it with

    PS2R_RECOMP_DIR=<build>/generated/games/slus_20909_crash_twinsanity/recomp

(custom commands through ${PS2R_RECOMP_ENV}, ctest rows through the ENVIRONMENT property). The
same for the recompiled IOP module (PS2R_IOP_RECOMP_DIR). Without the variable the historical
in-tree location under --root is used, so a developer who regenerates into the tree (it is
.gitignored) needs no variable.

Labels: the oracle generators record "games/<title>/recomp/<file>" as the provenance of each
extracted body; recomp_label() keeps that spelling whether the file came from the tree or the
build directory.
"""
import os
from pathlib import Path

TITLE = "slus_20909_crash_twinsanity"
RECOMP_REL = Path("games") / TITLE / "recomp"
IOP_RECOMP_REL = Path("games") / TITLE / "iop" / "stream_irx"
RECOMP_ENV = "PS2R_RECOMP_DIR"
IOP_RECOMP_ENV = "PS2R_IOP_RECOMP_DIR"


def recomp_dir(root) -> Path:
    """The directory holding recompiled.h / recompiled.cpp / the shards / continuations.h."""
    override = os.environ.get(RECOMP_ENV)
    return Path(override) if override else Path(root) / RECOMP_REL


def iop_recomp_dir(root) -> Path:
    """The directory holding iop_recompiled.h / iop_recompiled.cpp."""
    override = os.environ.get(IOP_RECOMP_ENV)
    return Path(override) if override else Path(root) / IOP_RECOMP_REL


def recomp_file(root, name) -> Path:
    return recomp_dir(root) / name


def shard_path(root, shard) -> Path:
    """recompiled_shard_NNN.cpp for an int or a 3-digit string."""
    if isinstance(shard, int):
        shard = f"{shard:03d}"
    return recomp_dir(root) / f"recompiled_shard_{shard}.cpp"


def shards(root):
    """Every shard, sorted."""
    return sorted(recomp_dir(root).glob("recompiled_shard_*.cpp"))


def recomp_label(path) -> str:
    """The tree-relative spelling of a generated file, for manifests / provenance labels."""
    return str(RECOMP_REL / Path(path).name)


def resolve(root, relative) -> Path:
    """root / relative, with a path under games/<title>/recomp/ redirected to the generated directory.

    The scripts keep spelling their inputs tree-relative ("games/.../recomp/recompiled_shard_009.cpp",
    "engine/ee/cpu/cpu_state.h"); only the generated ones move."""
    rel = Path(relative)
    # The inventories spell the title directory either by name or as the "<title>" placeholder
    # (docs/design/movie_caller_inventory.json): both name the configured title's generated output.
    for prefix in (RECOMP_REL, Path("games") / "<title>" / "recomp"):
        try:
            rest = rel.relative_to(prefix)
        except ValueError:
            continue
        return recomp_dir(root) / rest
    return Path(root) / rel


def label(path, root) -> str:
    """str(path.relative_to(root)) for a tree file; the tree-relative spelling for a generated one."""
    path = Path(path)
    try:
        return str(path.relative_to(Path(root)))
    except ValueError:
        return recomp_label(path)
