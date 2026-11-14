"""Build the small, dependency-free goreX import reference scene."""
import json
import math
from pathlib import Path
import struct
import zlib

blob = bytearray()
views, accessors, meshes, nodes = [], [], [], []
bake_surfaces = []

def view(data, target=None):
    blob.extend(b"\0" * (-len(blob) % 4))
    index = len(views)
    item = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
    if target is not None:
        item["target"] = target
    views.append(item)
    blob.extend(data)
    return index

def accessor(rows, fmt, component, shape, target, bounds=False):
    data = b"".join(struct.pack("<" + fmt, *row) for row in rows)
    item = {"bufferView": view(data, target), "componentType": component,
            "count": len(rows), "type": shape}
    if bounds:
        item["min"] = [min(column) for column in zip(*rows)]
        item["max"] = [max(column) for column in zip(*rows)]
    accessors.append(item)
    return len(accessors) - 1

def png(size, pixel):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data)))
    rows = b"".join(b"\0" + bytes(channel for x in range(size)
                       for channel in pixel(x, y)) for y in range(size))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows))
            + chunk(b"IEND", b""))

def concrete(x, y):
    noise = ((x * 6791) ^ (y * 3989) ^ (x * y * 29)) % 27
    tint = (x // 16 + y // 16) % 2 * 10
    v = 118 + noise + tint
    if x % 32 == 0 or y % 32 == 0:
        v //= 2
    return v, v, min(255, v + 5), 255

def add(a, b):
    return tuple(x+y for x,y in zip(a,b))

def sub(a, b):
    return tuple(x-y for x,y in zip(a,b))

def mul(a, factor):
    return tuple(x*factor for x in a)

def dot(a, b):
    return sum(x*y for x,y in zip(a,b))

def bake_bounce(filename, center, extent, emitters):
    """Offline one-bounce irradiance from diffuse floor and wall patches."""
    size, tile = 96, 32
    def pixel(x, y):
        face = x//tile + (y//tile)*3
        if face >= len(faces):
            return 0,0,0,255
        normal, vertices = faces[face]
        s = min(1.0,max(0.0, (x%tile-1.5)/(tile-4)))
        t = 1.0-min(1.0,max(0.0, (y%tile-1.5)/(tile-4)))
        local = add(vertices[0],add(mul(sub(vertices[1],vertices[0]),s),
                                    mul(sub(vertices[3],vertices[0]),t)))
        p = tuple(c+e*v*0.5 for c,e,v in zip(center,extent,local))
        energy = [0.014,0.017,0.022]
        for source, direction, radiance in emitters:
            offset = sub(source,p)
            squared = dot(offset,offset)
            if squared < 0.002:
                continue
            ray = mul(offset,1/math.sqrt(squared))
            cosine = max(0.0,dot(ray,normal))*max(0.0,-dot(ray,direction))
            form = cosine/(1+squared)
            for i in range(3):
                energy[i] += radiance[i]*form
        border = 0.72 + 0.28*min(1, min(s,t,1-s,1-t)*12)
        rgb = tuple(round(255*(1.055*min(1,c*border)**(1/2.4)-0.055)) for c in energy)
        return (*rgb,255)
    filename.write_bytes(png(size,pixel))

def diffuse_patches():
    patches = []
    for index, z in enumerate((0,-7,-14)):
        lamp = (0,2.89,z)
        color = (0.38,0.72,1) if index != 2 else (1,0.13,0.058)
        strength = 0.33 if index != 2 else 0.57
        for x in (-2.2,0,2.2):
            for dz in (-2.1,0,2.1):
                for p,n in (((x,0.02,z+dz),(0,1,0)),
                            ((-2.97,1.15+x*0.12,z+dz),(1,0,0)),
                            ((2.97,1.15+x*0.12,z+dz),(-1,0,0))):
                    ray = sub(lamp,p)
                    length2 = dot(ray,ray)
                    direction = mul(ray,1/math.sqrt(length2))
                    patch_energy = max(0,dot(n,direction)) * direction[1]**3
                    patches.append((p,n,mul(color,strength*patch_energy/(1+0.12*length2))))
    return patches

def grille(x, y):
    solid = x < 4 or y < 4 or x > 59 or y > 59 or x % 10 < 2 or y % 10 < 2
    return 145, 162, 174, 255 if solid else 0

images = [{"bufferView": view(png(64, pixel)), "mimeType": "image/png", "name": name}
          for name, pixel in (("concrete", concrete), ("grille", grille))]

def surface(name, color, roughness, metallic=0):
    return {"name": name, "pbrMetallicRoughness": {
        "baseColorFactor": [*color, 1], "roughnessFactor": roughness, "metallicFactor": metallic}}

materials = [
    surface("Wet concrete", (0.64, 0.73, 0.8), 0.36),
    surface("Painted steel", (0.14, 0.29, 0.35), 0.41, 0.45),
    surface("Safety orange", (0.75, 0.25, 0.055), 0.59),
    surface("Cold tube", (0.2, 0.6, 0.85), 0.4),
    surface("Cutout grille", (0.7, 0.8, 0.9), 0.48, 0.25),
    surface("Emergency tube", (0.85, 0.18, 0.07), 0.4),
]
materials[0]["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
materials[3]["emissiveFactor"] = [0.25, 0.75, 1.0]
materials[3]["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": 4}}
materials[4]["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 1}
materials[4].update(alphaMode="MASK", alphaCutoff=0.5, doubleSided=True)
materials[5]["emissiveFactor"] = [1.0, 0.12, 0.032]
materials[5]["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": 4.2}}

faces = [
    ((0,0,1), [(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]),
    ((0,0,-1), [(1,-1,-1),(-1,-1,-1),(-1,1,-1),(1,1,-1)]),
    ((1,0,0), [(1,-1,1),(1,-1,-1),(1,1,-1),(1,1,1)]),
    ((-1,0,0), [(-1,-1,-1),(-1,-1,1),(-1,1,1),(-1,1,-1)]),
    ((0,1,0), [(-1,1,1),(1,1,1),(1,1,-1),(-1,1,-1)]),
    ((0,-1,0), [(-1,-1,-1),(1,-1,-1),(1,-1,1),(-1,-1,1)]),
]

def mesh(name, selected, material):
    positions, normals, uv, uv1, indices = [], [], [], [], []
    for face, (normal, vertices) in enumerate(selected):
        i = len(positions)
        positions.extend(tuple(c * 0.5 for c in v) for v in vertices)
        normals.extend([normal] * 4)
        uv.extend([(0,0),(2,0),(2,2),(0,2)] if material == 0 else [(0,0),(1,0),(1,1),(0,1)])
        uv1.extend(((face%3+(2+28*s)/32)/3, (face//3+(2+28*t)/32)/3)
                   for s,t in ((0,1),(1,1),(1,0),(0,0)))
        indices.extend((i+a,) for a in (0,1,2,0,2,3))
    attributes = {
        "POSITION": accessor(positions, "fff", 5126, "VEC3", 34962, True),
        "NORMAL": accessor(normals, "fff", 5126, "VEC3", 34962),
        "TEXCOORD_0": accessor(uv, "ff", 5126, "VEC2", 34962),
        "TEXCOORD_1": accessor(uv1, "ff", 5126, "VEC2", 34962),
    }
    meshes.append({"name": name, "primitives": [{"attributes": attributes,
        "indices": accessor(indices, "H", 5123, "SCALAR", 34963), "material": material}]})
    return len(meshes) - 1

cubes = [mesh(material["name"], faces, i) for i, material in enumerate(materials)]
panel = mesh("Grille", faces[:1], 4)

def node(name, pos=(0,0,0), **extra):
    nodes.append({"name": name, "translation": pos, **extra})
    return len(nodes) - 1

def box(name, pos, scale, material=0, solid=True, angle=0):
    fields = {"scale": scale, "mesh": cubes[material]}
    if angle:
        fields["rotation"] = (math.sin(angle / 2), 0, 0, math.cos(angle / 2))
    if "floor" in name or name == "Side wall" or name == "Far wall":
        filename = f"import-lab-bounce-{len(bake_surfaces)}.png"
        bake_surfaces.append((filename,pos,scale))
        fields["extras"] = {"lightmap": filename, "lightmap_strength": 0.83}
    if "floor" in name:
        fields["extras"]["wetness"] = 0.84
    node(name, pos, **fields)
    if solid:
        node(("COL_MESH_" if angle else "COL_") + name, pos,
             **{k:v for k,v in fields.items() if k != 'extras'})

box("Entry floor", (0,-0.125,0.6), (6,0.25,7.2))
box("Central floor", (0,-0.125,-7), (6,0.25,8))
box("Far floor", (0,-0.125,-14.8), (6,0.25,7.6))
for x in (-3.1, 3.1):
    for z in (0.2, -6.9, -14.2):
        box("Side wall", (x,1.65,z), (0.2,3.5,7.5))
box("Rear wall", (0,1.6,4.3), (6.4,3.3,0.2))
box("Far wall", (0,1.6,-18.5), (6.4,3.3,0.2))
for z in (0, -7, -14):
    box("Ceiling", (0,3.5,z), (6.3,0.25,7.5), 1)
    box("Tube", (0,3.28,z), (2.1,0.08,0.12), 5 if z == -14 else 3, False)
for i in range(4):
    height = (i+1) * 0.19
    box("Step", (-1.8,height/2,-3.3-i*0.52), (1.65,height,0.53), 1)
box("Landing", (-1.8,0.38,-6.0), (1.65,0.76,2.4), 1)
angle = -math.atan2(0.76, 2.15)
box("Ramp", (-1.8,0.38-0.065,-8.285), (1.65,0.13,math.hypot(2.15,0.76)), 1, True, angle)
box("Low beam", (0,2.22,-10.25), (2.4,2.04,0.55), 1)
box("Left barrier", (-2.12,1.65,-10.25), (1.8,3.5,0.55), 1)
box("Right barrier", (2.12,1.65,-10.25), (1.8,3.5,0.55), 1)
box("PROP_Test crate", (0.95,0.65,-5.4), (0.72,0.72,0.72), 2, False)
box("PROP_Second crate", (-0.7,0.55,-14), (0.66,0.66,0.66), 2, False)
box("Thin divider", (2.0,1.0,-14.7), (1.9,2.0,0.04), 1)
node("Grille", (-1.6,1.5,-15.3), mesh=panel, scale=(2.4,2.4,1))
node("TRIGGER_exit", (0,0.8,-17.7), mesh=cubes[0], scale=(2,1.6,0.6))
node("SPAWN_player", (0,0.015,2.6))
node("SPAWN_display", (1.75,0.015,-3.1))
tour = [
    ((0,1.63,2.6),0.0), ((-0.65,1.64,-1.7),0.11), ((-1.7,2.15,-5.2),-0.22),
    ((0.22,1.16,-8.7),0.05), ((0.12,0.91,-10.2),-0.02), ((0.05,1.08,-11.55),-0.04),
    ((-0.5,1.55,-13.55),-0.16), ((-0.35,1.58,-16.3),-0.34),
]
for i, (pos, yaw) in enumerate(tour):
    node(f"SPAWN_camera_{i}", pos, rotation=(0,math.sin(yaw/2),0,math.cos(yaw/2)))

# Box scale sets the full size of each projected effect.
node("PROBE_Entry", (0,1.08,-2.6), scale=(6.6,3.55,12.5), extras={"strength":0.95})
node("PROBE_Emergency", (0,1.05,-13.25), scale=(6.6,3.5,12.8), extras={"strength":1.15})
node("FOG_ColdFloor", (0,0.30,-5.0), scale=(5.7,0.62,7.0),
     extras={"density":0.35,"color_r":0.025,"color_g":0.065,"color_b":0.105})
node("FOG_Emergency", (0,0.34,-14.5), scale=(5.7,0.70,6.9),
     extras={"density":0.43,"color_r":0.085,"color_g":0.020,"color_b":0.017})
wall_rotation = (0,-math.sin(math.pi/4),0,math.cos(math.pi/4))
floor_rotation = (-math.sin(math.pi/4),0,0,math.cos(math.pi/4))
node("DECAL_LEAK_Pipes", (2.985,1.65,-3.5), rotation=wall_rotation, scale=(2.35,2.75,0.17),
     extras={"strength":0.92,"wetness":0.65,"color_r":0.032,"color_g":0.044,"color_b":0.036})
node("DECAL_SCUFF_Footpath", (0.3,0,-6.2), rotation=floor_rotation, scale=(2.4,4.6,0.19),
     extras={"strength":0.82,"color_r":0.051,"color_g":0.041,"color_b":0.029})
node("DECAL_GRIME_Threshold", (0,0,-10.7), rotation=floor_rotation, scale=(2.25,2.6,0.15),
     extras={"strength":0.77,"wetness":0.7,"color_r":0.042,"color_g":0.029,"color_b":0.027})

for x in (-2.72,2.72):
    for z in (2.7,-1.5,-6.7,-13.0,-17.1):
        box("Steel upright", (x,1.67,z), (0.24,3.32,0.32), 1, False)
    box("Service conduit", (x,2.72,-3.1), (0.17,0.16,12.7), 1, False)
for z in (-8.3,-16):
    box("Service beam", (0,2.97,z), (5.7,0.25,0.28), 1, False)

lights = []
for i, z in enumerate((0, -7, -14)):
    color = (0.38,0.72,1.0) if i != 2 else (1,0.13,0.058)
    lights.append({"type":"spot","color":color,"intensity":65 if i != 2 else 95,"range":10,
                   "spot":{"innerConeAngle":0.48,"outerConeAngle":0.97}})
    node("Downlight", (0,2.89,z), rotation=floor_rotation, extras={"cast_shadow":True},
         extensions={"KHR_lights_punctual":{"light":i}})
lights.append({"type":"point","color":(1,0.18,0.075),"intensity":18,"range":6})
node("Emergency point light", (0.65,1.95,-16.8), extras={"cast_shadow":True},
     extensions={"KHR_lights_punctual":{"light":len(lights)-1}})

root = node("Import Lab", children=list(range(len(nodes))))
document = {
    "asset": {"version": "2.0", "generator": "goreX import lab generator"},
    "extensionsUsed": ["KHR_lights_punctual", "KHR_materials_emissive_strength"],
    "extensions": {"KHR_lights_punctual": {"lights": lights}},
    "scene": 0, "scenes": [{"nodes": [root]}], "nodes": nodes,
    "meshes": meshes, "materials": materials, "images": images,
    "textures": [{"source": i, "sampler": 0} for i in range(len(images))],
    "samplers": [{"minFilter": 9987, "magFilter": 9729, "wrapS": 10497, "wrapT": 10497}],
    "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(blob)}],
}
json_data = json.dumps(document, separators=(",", ":")).encode("utf-8")
json_data += b" " * (-len(json_data) % 4)
blob.extend(b"\0" * (-len(blob) % 4))
out = Path(__file__).resolve().parents[1] / "assets" / "test" / "import-lab.glb"
patches = diffuse_patches()
for filename, center, extent in bake_surfaces:
    bake_bounce(out.with_name(filename), center, extent, patches)
out.write_bytes(struct.pack("<4sII", b"glTF", 2, 28 + len(json_data) + len(blob))
                + struct.pack("<I4s", len(json_data), b"JSON") + json_data
                + struct.pack("<I4s", len(blob), b"BIN\0") + blob)
print(f"Wrote {out} ({out.stat().st_size} bytes)")
