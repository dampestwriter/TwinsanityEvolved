"""Strict readers for the source-declared material/resource inventory.

TR resources.cpp, shaderclasses.cpp and renderer.h are the read-only schema
authority. These readers classify file records; they execute no game packets.
"""
import struct


FIELDS = (
    "blend preset alpha_test alpha_method alpha_reference alpha_fail "
    "destination_test destination_mode depth_test unused23 gouraud textured stq "
    "fog context u_scroll v_scroll own_alpha alpha_a alpha_b alpha_c "
    "alpha_d alpha_fix linear no_fba unused57 anti_alias no_depth_write "
    "animated_colour has_animation"
).split()


class Record:
    def __init__(self, data, start, size):
        if start < 0 or size < 0 or start + size > len(data):
            raise ValueError("record extent")
        self.data, self.start, self.at, self.end = data, start, start, start + size

    def take(self, size):
        if size < 0 or size > self.end - self.at:
            raise ValueError("record field extent")
        at = self.at
        self.at += size
        return self.data[at:self.at]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def finish(self):
        if self.at != self.end:
            raise ValueError("unclassified record tail")


def material(data, start, size, complete=True):
    reader = Record(data, start, size)
    key, bucket, name_size = struct.unpack("<QII", reader.take(16))
    name = bytes(reader.take(name_size)).rstrip(b"\0").decode("ascii")
    count = reader.u32()
    if count > 4:
        raise ValueError("material shader count")
    shaders = []
    prefixes = {0x10: 4, 0x11: 4, 0x17: 12, 0x18: 4, 0x1a: 20, 0x1c: 8}
    for _ in range(count):
        kind = reader.u32()
        if kind > 0x20:
            raise ValueError("unknown shader class")
        reader.take(prefixes.get(kind, 0))
        fields = dict(zip(FIELDS, reader.take(30)))
        if fields["has_animation"]:
            raise ValueError("animated shader grammar requires source closure")
        lod = struct.unpack("<hh", reader.take(4))
        reader.take(16)  # tool leftover vector, not a resource reference
        colour = struct.unpack("<4f", reader.take(16))
        scroll = struct.unpack("<4f", reader.take(16))
        texture, repeated = struct.unpack("<II", reader.take(8))
        if kind != repeated:
            raise ValueError("shader class disagreement")
        shaders.append(dict(type=kind, settings=fields, lod=lod, colour=colour,
                            scroll=scroll, texture=texture))
    if complete:
        reader.finish()
    return dict(key=key, bucket=bucket, name=name, shaders=shaders), reader.at


def rigid(data, start, size):
    reader = Record(data, start, size)
    version, count = reader.u32(), reader.u32()
    if count > 4096:
        raise ValueError("rigid material extent")
    materials = [reader.u32() for _ in range(count)]
    model = reader.u32()
    reader.finish()
    return dict(version=version, materials=materials, model=model)


def skin(data, start, size):
    reader = Record(data, start, size)
    count = reader.u32()
    if count > 4096:
        raise ValueError("skin material extent")
    materials = []
    for _ in range(count):
        materials.append(reader.u32())
        length, _vertices = reader.u32(), reader.u32()
        reader.take(length)
    reader.finish()
    return materials


def blend(data, start, size):
    reader = Record(data, start, size)
    count, shapes = reader.u32(), reader.u32()
    if count > 4096 or shapes > 255:
        raise ValueError("blend skin extent")
    materials = []
    for _ in range(count):
        parts = reader.u32()
        materials.append(reader.u32())
        if parts > 4096:
            raise ValueError("blend part extent")
        for _ in range(parts):
            length, _unused = reader.u32(), reader.u32()
            reader.take(length)
            reader.take(12)  # source BlendPart::shapeFactors[3]
            for _ in range(shapes):
                quadwords, _vertices = reader.u32(), reader.u32()
                reader.take(quadwords * 16)
    reader.finish()
    return materials


def mesh_references(data, start, size, kind):
    reader = Record(data, start, size)
    version = reader.u32()
    if kind == 8:  # Sky: ignored word, count, mesh IDs
        count = reader.u32()
    elif version == 0x1001:
        count = reader.u32()
        reader.take(24)  # nearest/farthest, three switches, unused word
    elif version == 0x1002:
        count = reader.take(1)[0]
        reader.take(20)
    else:
        raise ValueError("unclassified LOD version")
    if count > 4096:
        raise ValueError("mesh reference extent")
    meshes = [reader.u32() for _ in range(count)]
    reader.finish()
    return meshes


def model_parts(data, start, size):
    reader = Record(data, start, size)
    count = reader.u32()
    if count > 4096:
        raise ValueError("model part extent")
    parts = []
    for _ in range(count):
        vertices, length = reader.u32(), reader.u32()
        at = reader.at
        reader.take(length)
        parts.append(dict(vertices=vertices, offset=at, size=length))
        reader.take(reader.u32())  # source tool leftovers after each part
    reader.finish()
    return parts


def type01(record):
    """Material-side MeshPass envelope; geometry/other consumers are separate."""
    if record["key"] != 8 or len(record["shaders"]) != 1 or record["bucket"] >= 28:
        return False
    shader = record["shaders"][0]
    settings = shader["settings"]
    if shader["type"] != 1 or not shader["texture"]:
        return False
    if any(settings[name] != value for name, value in
           dict(gouraud=1, textured=1, stq=1, fog=0, context=0, anti_alias=0, has_animation=0).items()):
        return False
    if settings["blend"]:
        if settings["own_alpha"]:
            if any(settings[name] > 2 for name in ("alpha_a", "alpha_b", "alpha_c", "alpha_d")):
                return False
        elif settings["preset"] > 5:
            return False
    return True
