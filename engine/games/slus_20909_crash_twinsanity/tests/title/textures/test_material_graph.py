#!/usr/bin/env python3
"""Pins for retained consumers and whole-model (not per-shader) selection."""
import copy
import struct
import unittest

from material_graph import close, texture_scope
from material_records import FIELDS, material, model_parts, rigid, type01


def shader_record(texture=42, kind=1):
    fields = dict.fromkeys(FIELDS, 0)
    fields.update(gouraud=1, textured=1, stq=1, linear=1, preset=2,
                  alpha_test=1, destination_test=1, destination_mode=0)
    return dict(key=8, bucket=4, name="fixture",
                shaders=[dict(type=kind, texture=texture, settings=fields,
                              lod=(-1, -1), colour=(1, 1, 1, 1), scroll=(0, 0, 0, 0))])


def variants(record):
    return {"original": dict(record=record)}


class MaterialGraphTests(unittest.TestCase):
    def test_mixed_model_transitive_closure(self):
        mats = {i: variants(shader_record()) for i in (1, 2, 3, 4)}
        mats[4] = variants(shader_record(kind=2))
        models = {i: {"g" + str(i): dict(parts=[{}, {}])} for i in (1, 2, 3)}
        draws = [dict(model=i, materials=[i, i + 1]) for i in (1, 2, 3)]
        owned, reasons = close(mats, draws, models, {}, {"g1", "g2", "g3"})
        self.assertEqual(owned, set())
        self.assertEqual(reasons[1], {"shared-with-unowned-model"})

    def test_every_retained_consumer_overrides_shader_type(self):
        mats = {1: variants(shader_record()), 2: variants(shader_record())}
        models = {3: {"g": dict(parts=[{}, {}])}}
        draws = [dict(model=3, materials=[1, 2])]
        for family in ("shadow-default", "skin", "blend-skin", "sky", "sprite", "font",
                       "particle", "potential-borrowed-hud", "opaque-reference"):
            with self.subTest(family=family):
                owned, reasons = close(mats, draws, models, {2: {family}}, {"g"})
                self.assertEqual(owned, set())
                self.assertIn(family, reasons[2])
                self.assertIn("shared-with-unowned-model", reasons[1])

    def test_unseen_variant_and_extra_geometry_part(self):
        mats = {1: variants(shader_record())}
        # NEW model load decodes ALL parts, including one with no material slot.
        models = {2: {"good": dict(parts=[{}]), "other": dict(parts=[{}, {}])}}
        draws = [dict(model=2, materials=[1])]
        self.assertEqual(close(mats, draws, models, {}, {"good"})[0], set())
        self.assertEqual(close(mats, draws, models, {}, {"good", "other"})[0], {1})
        mats[1]["different-family"] = dict(record=shader_record(kind=13))
        self.assertEqual(close(mats, draws, models, {}, {"good", "other"})[0], set())

    def test_missing_geometry_or_material_never_success(self):
        mats = {1: variants(shader_record())}
        self.assertEqual(close(mats, [dict(model=2, materials=[1])], {}, {}, set())[0], set())
        owned, reasons = close(mats, [dict(model=2, materials=[1, 7])],
                               {2: {"g": dict(parts=[{}, {}])}}, {}, {"g"})
        self.assertEqual(owned, set())
        self.assertIn("missing-material", reasons[7])

    def test_texture_has_independent_retained_consumers(self):
        mats = {1: variants(shader_record()), 2: variants(shader_record(kind=13))}
        self.assertFalse(texture_scope(mats, {1}, set())[42])
        self.assertTrue(texture_scope(mats, {1, 2}, set())[42])
        self.assertFalse(texture_scope(mats, {1, 2}, {42})[42])
        changed = copy.deepcopy(mats[1]["original"])
        changed["record"]["shaders"][0]["texture"] = 43
        mats[1]["other-load-generation"] = changed
        _, reasons = close(mats, [], {}, {}, set())
        self.assertIn("texture-identity-varies", reasons[1])

    def test_old_signed_lod_date_and_subtract_are_not_sanitized(self):
        # OLD mip/DATE/subtract pins: source settings are preserved. Selection
        # may not "fix" them by dropping tests, changing K/L or enabling fog.
        record = shader_record()
        s = record["shaders"][0]
        blob = (struct.pack("<QII", 8, 4, 2) + b"x\0" + struct.pack("<II", 1, 1) +
                bytes(s["settings"][name] for name in FIELDS) +
                struct.pack("<hh", -16, -1) + bytes(16) +
                struct.pack("<8fII", *s["colour"], *s["scroll"], 42, 1))
        parsed, end = material(blob, 0, len(blob))
        self.assertEqual(end, len(blob))
        self.assertEqual(parsed["shaders"][0]["lod"], (-16, -1))
        self.assertEqual(parsed["shaders"][0]["settings"]["preset"], 2)
        self.assertEqual(parsed["shaders"][0]["settings"]["destination_test"], 1)
        self.assertTrue(type01(parsed))
        parsed["shaders"][0]["settings"]["fog"] = 1
        self.assertFalse(type01(parsed))
        for changed in (blob[:-1], blob + b"\0"):
            with self.assertRaises(ValueError):
                material(changed, 0, len(changed))

    def test_source_model_bounds_and_rigid_reference_counts(self):
        blob = struct.pack("<IIIII", 0x1001, 2, 10, 11, 99)
        self.assertEqual(rigid(blob, 0, len(blob))["materials"], [10, 11])
        model = struct.pack("<III", 1, 3, 4) + b"1234" + struct.pack("<I", 0)
        self.assertEqual(model_parts(model, 0, len(model)), [dict(vertices=3, offset=12, size=4)])
        for bad in (model[:-1], model + b"\0"):
            with self.assertRaises(ValueError):
                model_parts(bad, 0, len(bad))


if __name__ == "__main__":
    unittest.main()
