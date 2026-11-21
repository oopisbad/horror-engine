"""Editable map documents, placement coordinates and logic briefs."""

import json
import math
import os
import tempfile
import uuid
from pathlib import Path


def uid():
    return uuid.uuid4().hex


def dump(path, data):
    path = Path(path)
    payload = json.dumps(data, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temp:
        name = temp.name
        try:
            temp.write(payload.encode("utf-8"))
        except BaseException:
            temp.close()
            os.unlink(name)
            raise
    try:
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def item(kind, name, pos=(0, 0, 0), scale=(1, 1, 1)):
    return {
        "id": uid(),
        "kind": kind,
        "name": name,
        "position": list(pos),
        "scale": list(scale),
        "yaw": 0.0,
        "notes": [],
    }


def new_map():
    return {
        "version": 1,
        "id": uid(),
        "objects": [
            item("block", "Floor", (0, -0.25, 0), (12, 0.5, 12)),
            item("spawn", "Player", (0, 0.02, 3)),
        ],
    }


def rotate(x, z, yaw):
    a = math.radians(yaw)
    return (math.cos(a) * x + math.sin(a) * z, -math.sin(a) * x + math.cos(a) * z)


def world(obj, local):
    x, y, z = [a * b for a, b in zip(local, obj["scale"])]
    x, z = rotate(x, z, obj["yaw"])
    return [a + b for a, b in zip((x, y, z), obj["position"])]


def local(obj, point):
    x, y, z = [a - b for a, b in zip(point, obj["position"])]
    x, z = rotate(x, z, -obj["yaw"])
    return [a / b for a, b in zip((x, y, z), obj["scale"])]


def vector(values):
    if len(values) != 3 or not all(
        isinstance(n, (float, int)) and math.isfinite(n) for n in values
    ):
        raise ValueError("Expected three finite coordinates")
    return list(values)


def validate(doc):
    if doc["version"] != 1 or not isinstance(doc["objects"], list):
        raise ValueError("Unsupported map document")
    ids = set()

    def check_id(value):
        if (
            not isinstance(value, str)
            or len(value) != 32
            or any(c not in "0123456789abcdef" for c in value)
            or value in ids
        ):
            raise ValueError("Missing or duplicate map ID")
        ids.add(value)

    check_id(doc["id"])
    for obj in doc["objects"]:
        check_id(obj["id"])
        if obj["kind"] not in ("block", "trigger", "spawn", "model") or not obj["name"].strip():
            raise ValueError("Invalid object kind or empty name")
        vector(obj["position"])
        if min(vector(obj["scale"])) <= 0 or not math.isfinite(obj["yaw"]):
            raise ValueError("Scale must be positive and yaw must be finite")
        if obj["kind"] == "model" and not isinstance(obj["asset"], str):
            raise ValueError("Invalid model path")
        for note in obj["notes"]:
            check_id(note["id"])
            vector(note["local"])
            if not note["logic"].strip():
                raise ValueError("A logic note needs a description")
    if sum(o["kind"] == "spawn" for o in doc["objects"]) > 1:
        raise ValueError("Only one player spawn is supported")


def load(path):
    doc = json.loads(path.read_text(encoding="utf-8-sig"))
    validate(doc)
    for obj in doc["objects"]:
        if obj["kind"] == "model":
            obj["asset"] = str((path.parent / obj["asset"]).resolve())
    return doc


def relative(asset, directory):
    try:
        return Path(os.path.relpath(asset, directory)).as_posix()
    except ValueError:  # Different Windows drives.
        return Path(asset).as_posix()


def save(path, doc):
    validate(doc)
    objects = [
        dict(o, asset=relative(o["asset"], path.parent)) if o["kind"] == "model" else o
        for o in doc["objects"]
    ]
    dump(path, dict(doc, objects=objects))


def runtime_name(obj):
    if obj["kind"] == "spawn":
        return "SPAWN_player"
    prefix = "TRIGGER_" if obj["kind"] == "trigger" else "Map_"
    return prefix + obj["id"]


def brief(path, doc):
    validate(doc)
    objects = []
    for o in doc["objects"]:
        if not o["notes"]:
            continue
        target = {k: o[k] for k in ("id", "kind", "name", "position", "scale", "yaw")}
        target["node"] = runtime_name(o)
        if "asset" in o:
            target["asset"] = relative(o["asset"], path.parent)
        target["notes"] = [dict(n, world=world(o, n["local"])) for n in o["notes"]]
        objects.append(target)
    dump(
        path,
        {
            "version": 1,
            "map_id": doc["id"],
            "coordinates": "Metres, Y up, yaw in degrees about +Y",
            "objects": objects,
        },
    )
