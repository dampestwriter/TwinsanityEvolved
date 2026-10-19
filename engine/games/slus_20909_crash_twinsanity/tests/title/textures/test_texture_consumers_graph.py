#!/usr/bin/env python3
"""Pins for the per-texture consumer family closure (pure graph, no archive)."""
import unittest

from audit_texture_consumers import FAMILIES, bit, classify


def shader(texture, kind=1):
    return dict(type=kind, texture=texture)


def material(*shaders):
    return {"original": dict(record=dict(shaders=list(shaders)))}


class TextureConsumerGraphTests(unittest.TestCase):
    def test_family_contract_order_is_stable(self):
        # Bit positions are the C++ TextureConsumers::Family contract.
        self.assertEqual(FAMILIES[:3], ["certified-placed", "placed", "rigid"])
        self.assertEqual(FAMILIES[11], "cloth")
        self.assertEqual(FAMILIES[12:], ["opaque-reference", "unreferenced", "missing-record",
                                         "orphan-material"])
        self.assertEqual(len(FAMILIES), 16)

    def test_certified_material_is_not_also_plain_placed(self):
        mats = {1: material(shader(10)), 2: material(shader(11))}
        draws = [dict(kind=6, materials=[1]), dict(kind=6, materials=[2])]
        textures = classify(mats, draws, {}, [], {1}, {10, 11}, set())
        self.assertEqual(textures[10]["families"], bit("certified-placed"))
        self.assertEqual(textures[11]["families"], bit("placed"))
        self.assertEqual(textures[10]["shaders"], 1 << 1)

    def test_shared_texture_accumulates_every_family(self):
        mats = {1: material(shader(10)), 2: material(shader(10, 0x17)), 3: material(shader(10, 2))}
        draws = [dict(kind=6, materials=[1]), dict(kind=6, materials=[2]), dict(kind=3, materials=[3])]
        excluded = {3: {"potential-borrowed-hud"}, 2: {"skin"}}
        textures = classify(mats, draws, excluded, [(10, "sprite")], {1}, {10}, set())
        expected = (bit("certified-placed") | bit("placed") | bit("rigid") | bit("cloth")
                    | bit("skin") | bit("borrowed-hud") | bit("sprite"))
        self.assertEqual(textures[10]["families"], expected)
        self.assertEqual(textures[10]["shaders"], (1 << 1) | (1 << 0x17) | (1 << 2))
        self.assertEqual(textures[10]["materials"], {1, 2, 3})

    def test_unownable_families(self):
        mats = {1: material(shader(10)), 2: material(shader(20))}
        draws = [dict(kind=6, materials=[1])]
        textures = classify(mats, draws, {}, [], set(), {10, 30}, {10})
        self.assertEqual(textures[10]["families"], bit("placed") | bit("opaque-reference"))
        # Material 2 has no consumer at all and names a texture absent from the archive.
        self.assertEqual(textures[20]["families"], bit("orphan-material") | bit("missing-record"))
        self.assertEqual(textures[30]["families"], bit("unreferenced"))

    def test_certified_without_placed_consumer_is_an_error(self):
        mats = {1: material(shader(10))}
        with self.assertRaises(ValueError):
            classify(mats, [], {}, [], {1}, {10}, set())

    def test_header_pairs_without_materials(self):
        textures = classify({}, [], {}, [(5, "font"), (6, "particle"), (5, "sprite")], set(), {5, 6}, set())
        self.assertEqual(textures[5]["families"], bit("font") | bit("sprite"))
        self.assertEqual(textures[6]["families"], bit("particle"))
        self.assertEqual(textures[5]["shaders"], 0)


if __name__ == "__main__":
    unittest.main()
