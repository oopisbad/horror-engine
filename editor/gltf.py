"""Compose engine-ready glTF 2 levels without changing source assets."""

import base64
import copy
import hashlib
import json
import math
import mimetypes
import struct
from pathlib import Path
from urllib.parse import unquote, urlsplit

import rotation
from maps import PROPERTIES, dump, runtime_name, validate

TABLES = (
    "accessors",
    "animations",
    "buffers",
    "bufferViews",
    "cameras",
    "images",
    "materials",
    "meshes",
    "nodes",
    "samplers",
    "skins",
    "textures",
)
SUPPORTED = {
    "KHR_lights_punctual",
    "KHR_materials_emissive_strength",
    "KHR_materials_unlit",
    "KHR_texture_transform",
    "KHR_mesh_quantization",
}


def read_asset(path):
    raw = path.read_bytes()
    binary = None
    if raw.startswith(b"glTF"):
        if len(raw) < 20 or struct.unpack_from("<II", raw, 4) != (2, len(raw)):
            raise ValueError("Invalid GLB header")
        at, doc = 12, None
        while at < len(raw):
            length, kind = struct.unpack_from("<I4s", raw, at)
            at += 8
            if at + length > len(raw):
                raise ValueError("Truncated GLB")
            chunk = raw[at : at + length]
            if kind == b"JSON":
                doc = json.loads(chunk)
            elif kind == b"BIN\0":
                binary = chunk
            at += length
    else:
        doc = json.loads(raw.decode("utf-8-sig"))
    if not doc or doc["asset"]["version"] != "2.0":
        raise ValueError("Expected a glTF 2.0 model")
    unknown = set(doc.get("extensionsUsed", [])) - SUPPORTED
    if unknown:
        raise ValueError("Unsupported import extensions: " + ", ".join(sorted(unknown)))

    for group in ("buffers", "images"):
        for entry in doc.get(group, []):
            uri = entry.get("uri")
            if uri and uri.startswith("data:"):
                continue
            if uri:
                parsed = urlsplit(uri)
                if parsed.scheme or parsed.netloc:
                    raise ValueError("Import assets with local resources")
                file = path.parent / unquote(parsed.path)
                data = file.read_bytes()
                mime = mimetypes.guess_type(file.name)[0]
            elif group == "buffers" and binary is not None:
                data = binary[: entry["byteLength"]]
                mime = None
            elif group == "images" and "bufferView" in entry:
                continue
            else:
                raise ValueError("Missing image or buffer data")
            entry["uri"] = (
                "data:"
                + (mime or "application/octet-stream")
                + ";base64,"
                + base64.b64encode(data).decode()
            )
    return doc


def cube():
    vertices, normals, indices = [], [], []
    for axis in range(3):
        for side in (-1, 1):
            start = len(vertices) // 3
            for u, v in [(-1, -1), (1, -1), (1, 1), (-1, 1)]:
                point = [0.0] * 3
                point[axis], point[(axis + 1) % 3], point[(axis + 2) % 3] = (
                    side * 0.5,
                    u * side * 0.5,
                    v * 0.5,
                )
                vertices.extend(point)
                normals.extend(side if i == axis else 0 for i in range(3))
            indices.extend(start + i for i in (0, 1, 2, 0, 2, 3))
    blob = struct.pack("<144f36H", *vertices, *normals, *indices)
    return {
        "buffers": [
            {
                "byteLength": len(blob),
                "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode(),
            }
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": o, "byteLength": l}
            for o, l in ((0, 288), (288, 288), (576, 72))
        ],
        "accessors": [
            {
                "bufferView": 0,
                "componentType": 5126,
                "count": 24,
                "type": "VEC3",
                "min": [-0.5] * 3,
                "max": [0.5] * 3,
            },
            {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": 36, "type": "SCALAR"},
        ],
        "materials": [
            {
                "name": "Blockout concrete",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.48, 0.53, 0.56, 1],
                    "metallicFactor": 0,
                    "roughnessFactor": 0.8,
                },
            }
        ],
        "meshes": [
            {
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "NORMAL": 1},
                        "indices": 2,
                        "material": 0,
                    }
                ]
            }
        ],
    }


def merge(dst, source, asset, output):
    src = copy.deepcopy(source)
    offsets = {k: len(dst.get(k, [])) for k in TABLES}

    def ref(obj, key, table):
        if isinstance(obj, list) or key in obj:
            index = obj[key]
            if not isinstance(index, int) or not 0 <= index < len(src.get(table, [])):
                raise ValueError(f"Invalid {table} index in {asset.name}")
            obj[key] = index + offsets[table]

    def refs(obj, key, table):
        if key in obj:
            for i in range(len(obj[key])):
                ref(obj[key], i, table)

    for v in src.get("bufferViews", []):
        ref(v, "buffer", "buffers")
    for a in src.get("accessors", []):
        ref(a, "bufferView", "bufferViews")
        if "sparse" in a:
            for key in ("indices", "values"):
                ref(a["sparse"][key], "bufferView", "bufferViews")
    for i in src.get("images", []):
        ref(i, "bufferView", "bufferViews")
    for t in src.get("textures", []):
        ref(t, "source", "images")
        ref(t, "sampler", "samplers")
    for m in src.get("materials", []):
        for parent in (m, m.get("pbrMetallicRoughness", {})):
            for key, value in parent.items():
                if key.endswith("Texture"):
                    ref(value, "index", "textures")
    for mesh in src.get("meshes", []):
        for p in mesh["primitives"]:
            for attrs in [p["attributes"], *p.get("targets", [])]:
                for key in attrs:
                    ref(attrs, key, "accessors")
            ref(p, "indices", "accessors")
            ref(p, "material", "materials")
    for skin in src.get("skins", []):
        ref(skin, "inverseBindMatrices", "accessors")
        ref(skin, "skeleton", "nodes")
        refs(skin, "joints", "nodes")
    for anim in src.get("animations", []):
        for s in anim["samplers"]:
            ref(s, "input", "accessors")
            ref(s, "output", "accessors")
        for c in anim["channels"]:
            ref(c["target"], "node", "nodes")
    lights = src.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", [])
    if lights:
        target = (
            dst.setdefault("extensions", {})
            .setdefault("KHR_lights_punctual", {})
            .setdefault("lights", [])
        )
        base = len(target)
        target.extend(lights)
    for n in src.get("nodes", []):
        refs(n, "children", "nodes")
        for key, table in (
            ("mesh", "meshes"),
            ("skin", "skins"),
            ("camera", "cameras"),
        ):
            ref(n, key, table)
        lamp = n.get("extensions", {}).get("KHR_lights_punctual")
        if lamp:
            if not 0 <= lamp["light"] < len(lights):
                raise ValueError("Invalid light reference")
            lamp["light"] += base
        extras = n.get("extras")
        if isinstance(extras, dict) and extras.get("lightmap"):
            lightmap = asset.parent / extras["lightmap"]
            data = lightmap.read_bytes()
            folder = output.parent / "map-lightmaps"
            folder.mkdir(exist_ok=True)
            copied = folder / (hashlib.sha256(data).hexdigest() + lightmap.suffix)
            if not copied.exists():
                copied.write_bytes(data)
            extras["lightmap"] = copied.relative_to(output.parent).as_posix()
    scenes = src.get("scenes", [])
    roots = scenes[src.get("scene", 0)].get("nodes", []) if scenes else []
    for k in TABLES:
        if src.get(k):
            dst.setdefault(k, []).extend(src[k])
    for k in ("extensionsUsed", "extensionsRequired"):
        if src.get(k):
            dst[k] = sorted(set(dst.get(k, [])) | set(src[k]))
    return [r + offsets["nodes"] for r in roots]


def apply_surface(material, properties):
    if "roughness" in properties:
        material.setdefault("pbrMetallicRoughness", {})["roughnessFactor"] = properties["roughness"]
    if "normal_strength" in properties and "normalTexture" in material:
        material["normalTexture"]["scale"] = properties["normal_strength"]


def export(path, doc):
    validate(doc)
    if not doc["objects"]:
        raise ValueError("Add at least one object before exporting a playable map.")
    out = dict(
        asset={"version": "2.0", "generator": "goreX Map Editor"},
        scene=0,
        scenes=[{"nodes": []}],
        nodes=[],
        extras={"map_id": doc["id"]},
        **cube(),
    )

    def node(fields):
        idx = len(out["nodes"])
        out["nodes"].append(fields)
        return idx

    for o in sorted(doc["objects"], key=lambda o: o["kind"] != "spawn"):
        q = rotation.quaternion(rotation.orientation(o))
        root = {
            "name": runtime_name(o),
            "translation": o["position"],
            "scale": o["scale"],
            "rotation": q,
            "extras": {"editor_id": o["id"], "editor_label": o["name"]},
        }
        kind = o["kind"]
        if kind in ("block", "trigger"):
            root["mesh"] = 0
        props = o.get("properties", {})
        if kind in ("spot", "fog", "leak", "chip"):
            values = {k: props.get(k, spec[1]) for k, spec in PROPERTIES[kind].items()}
            if kind == "spot":
                lights = (
                    out.setdefault("extensions", {})
                    .setdefault("KHR_lights_punctual", {})
                    .setdefault("lights", [])
                )
                root["extensions"] = {"KHR_lights_punctual": {"light": len(lights)}}
                outer = math.radians(values["cone"])
                lights.append(
                    {
                        "type": "spot",
                        "intensity": values["intensity"],
                        "range": values["range"],
                        "color": [values["color_" + c] for c in "rgb"],
                        "spot": {
                            "outerConeAngle": outer,
                            "innerConeAngle": outer * 0.7,
                        },
                    }
                )
                out["extensionsUsed"] = sorted(
                    set(out.get("extensionsUsed", [])) | {"KHR_lights_punctual"}
                )
                root["extras"]["cast_shadow"] = True
            else:
                root["extras"].update(values)
        if kind == "block":
            if props:
                mat = copy.deepcopy(out["materials"][0])
                apply_surface(mat, props)
                mesh = copy.deepcopy(out["meshes"][0])
                mesh["primitives"][0]["material"] = len(out["materials"])
                out["materials"].append(mat)
                root["mesh"] = len(out["meshes"])
                out["meshes"].append(mesh)
            root["children"] = [node({"name": "COL_" + o["id"], "mesh": 0})]
        if kind == "model":
            asset = Path(o["asset"])
            src = read_asset(asset)
            if props:
                materials = src.setdefault("materials", [])
                default = None
                for mesh in src.get("meshes", []):
                    for primitive in mesh["primitives"]:
                        if "material" not in primitive:
                            if default is None:
                                default = len(materials)
                                materials.append({})
                            primitive["material"] = default
                for material in materials:
                    apply_surface(material, props)
            root["children"] = merge(out, src, asset, path)
        out["scenes"][0]["nodes"].append(node(root))
    dump(path, out)
