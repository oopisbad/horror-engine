"""Plan-view footprints of the active glTF scene, in model-local X/Z."""

from itertools import product
from pathlib import Path

from gltf import read_asset


def transform(node, p):
    if "matrix" in node:
        m = node["matrix"]
        return tuple(sum(m[c * 4 + r] * p[c] for c in range(3)) + m[12 + r] for r in range(3))
    x, y, z = [a * b for a, b in zip(p, node.get("scale", (1, 1, 1)))]
    i, j, k, w = node.get("rotation", (0, 0, 0, 1))
    a, b, c = 2 * (j * z - k * y), 2 * (k * x - i * z), 2 * (i * y - j * x)
    return tuple(
        v + t
        for v, t in zip(
            (x + w * a + j * c - k * b, y + w * b + k * a - i * c, z + w * c + i * b - j * a),
            node.get("translation", (0, 0, 0)),
        )
    )


def hull(points):
    points = sorted(set(points))

    def half(seq):
        out = []
        for p in seq:
            while len(out) > 1:
                a, b = out[-2:]
                if (b[0] - a[0]) * (p[1] - a[1]) > (b[1] - a[1]) * (p[0] - a[0]):
                    break
                out.pop()
            out.append(p)
        return out

    return half(points)[:-1] + half(reversed(points))[:-1]


def footprints(path):
    d = read_asset(Path(path))
    nodes = d.get("nodes", [])
    scenes = d.get("scenes", [])
    if not scenes:
        raise ValueError("The asset needs a glTF scene")
    shapes = []

    def visit(idx, ancestors):
        if idx in ancestors:
            raise ValueError("Cyclic glTF node hierarchy")
        n = nodes[idx]
        chain = [idx, *ancestors]
        if "mesh" in n:
            for prim in d["meshes"][n["mesh"]]["primitives"]:
                bounds = d["accessors"][prim["attributes"]["POSITION"]]
                pts = []
                for p in product(*zip(bounds["min"], bounds["max"])):
                    for ancestor in chain:
                        p = transform(nodes[ancestor], p)
                    pts.append((p[0], p[2]))
                polygon = hull(pts)
                if len(polygon) >= 3:
                    shapes.append((idx, n.get("name", f"Node {idx}"), polygon))
        for child in n.get("children", []):
            visit(child, chain)

    for root in scenes[d.get("scene", 0)].get("nodes", []):
        visit(root, [])
    return shapes
