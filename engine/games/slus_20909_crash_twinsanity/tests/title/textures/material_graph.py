"""Whole-resource closure, independent of archive I/O and runtime observations."""
from collections import defaultdict

from material_records import type01


def close(materials, draws, models, excluded, qualified):
    """A material can be owned only if every model which writes it is owned.

    `qualified` contains complete ModelRead record digests accepted by the actual
    MeshAssets geometry envelope, including every part, not sampled parts.
    Runtime Append is all-or-nothing, hence the fixed point over mixed models.
    No positive geometry evidence means no positive certificate.
    """
    reasons = defaultdict(set)
    uses = defaultdict(list)
    for mid, variants in materials.items():
        if not variants or any(not type01(v["record"]) for v in variants.values()):
            reasons[mid].add("material-envelope")
        textures = {s["texture"] for v in variants.values() for s in v["record"]["shaders"]}
        if len(textures) != 1:
            reasons[mid].add("texture-identity-varies")
        reasons[mid].update(excluded.get(mid, ()))
    for draw in draws:
        mids = draw["materials"]
        for mid in mids:
            uses[mid].append(draw)
            if mid not in materials:
                reasons[mid].add("missing-material")
        variants = models.get(draw["model"], {})
        # MeshAssets decodes every part; Append needs one part per material.
        valid = bool(mids) and bool(variants) and all(
            len(v["parts"]) >= len(mids) and sha in qualified
            for sha, v in variants.items())
        if not valid:
            for mid in mids:
                reasons[mid].add("model-not-qualified")
    for mid in materials:
        if mid not in uses:
            reasons[mid].add("no-rigid-or-placed-consumer")
    changed = True
    while changed:
        changed = False
        for draw in draws:
            if any(reasons[mid] for mid in draw["materials"]):
                for mid in draw["materials"]:
                    if not reasons[mid]:
                        reasons[mid].add("shared-with-unowned-model")
                        changed = True
    return {mid for mid in materials if not reasons[mid]}, reasons


def texture_scope(materials, owned, foreign_textures):
    consumers = defaultdict(set)
    for mid, variants in materials.items():
        for v in variants.values():
            for shader in v["record"]["shaders"]:
                if shader["texture"]:
                    consumers[shader["texture"]].add(mid)
    return {tid: bool(mids) and mids <= owned and tid not in foreign_textures
            for tid, mids in consumers.items()}
