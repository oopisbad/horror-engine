"""Native solid viewport, camera navigation and modal object tools."""

import copy
import ctypes as ct
import itertools
import json
import math
import tkinter as tk
from pathlib import Path

import gltf
import maps
import rotation

F = ct.c_float
I = ct.c_int
P = ct.c_void_p
FP = ct.POINTER(F)
IP = ct.POINTER(I)
V3 = F * 3
V4 = F * 4
TOOLS = ("Move", "Rotate", "Scale")


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


class Viewport(tk.Frame):
    def __init__(self, parent, editor):
        super().__init__(parent, bg="#13171b", takefocus=True)
        self.editor = editor
        path = Path(__file__).resolve().parents[1] / "build-windows" / "gorex_viewport.dll"
        if not path.exists():
            raise OSError("Build the 3D editor first with build-windows.bat.")
        self.gl = ct.CDLL(str(path))
        for name, result, args in [
            ("init", I, [P]),
            ("close", None, []),
            ("load", P, [ct.c_char_p, ct.c_size_t]),
            ("free", None, [P]),
            ("bounds", I, [P, FP]),
            ("begin", None, [I, I, FP, FP, I]),
            ("object", None, [I, I, P, FP, FP, FP, I]),
            ("pin", None, [I, I, FP, I]),
            ("gizmo", None, [I, I, FP, FP, I]),
            ("end", None, []),
            ("hit", I, [I, I, IP, FP]),
        ]:
            fn = getattr(self.gl, "vp_" + name)
            fn.argtypes = args
            fn.restype = result
        if not self.gl.vp_init(self.winfo_id()):
            self.gl.vp_close()
            raise OSError("Windows could not create an OpenGL editor viewport.")
        self.models = {}
        self.bounds = {}
        self.center = [0.0, 0.0, 0.0]
        self.distance = 18.0
        self.azimuth = 0.62
        self.elevation = 0.48
        self.tool = 0
        self.session = None
        self.pending = None
        self.navigation = None
        self.bind("<Configure>", lambda e: self.redraw())
        self.bind("<Expose>", lambda e: self.redraw())
        self.bind("<Button-1>", self.click)
        self.bind("<ButtonRelease-1>", self.release)
        self.bind("<Motion>", self.motion)
        self.bind("<Button-2>", self.orbit_start)
        self.bind("<B2-Motion>", self.orbit)
        self.bind("<ButtonRelease-2>", lambda e: setattr(self, "navigation", None))
        self.bind("<MouseWheel>", self.wheel)
        self.bind("<Button-3>", lambda e: self.cancel())
        self.bind("<KeyPress>", self.key)
        self.bind("<FocusOut>", lambda e: self.cancel())
        self.bind("<Destroy>", self.close)

    def close(self, event):
        if self.pending:
            self.after_cancel(self.pending)
        for m in self.models.values():
            self.gl.vp_free(m)
        self.gl.vp_close()

    def forget(self, path):
        old = self.models.pop(str(path), None)
        if old:
            self.gl.vp_free(old)
        self.bounds.pop(str(path), None)

    def load(self, path):
        data = gltf.read_asset(Path(path))
        encoded = json.dumps(data, separators=(",", ":")).encode()
        model = self.gl.vp_load(encoded, len(encoded))
        if not model:
            raise ValueError(f"Could not load scene geometry from {Path(path).name}")
        self.forget(path)
        self.models[str(path)] = model
        bounds = (F * 6)()
        if self.gl.vp_bounds(model, bounds):
            self.bounds[str(path)] = tuple(bounds)
        return [
            (i, n.get("name", f"Node {i}"))
            for i, n in enumerate(data.get("nodes", []))
            if "mesh" in n
        ]

    def basis(self):
        a, b = self.azimuth, self.elevation
        right = (math.cos(a), 0, -math.sin(a))
        back = (math.sin(a) * math.cos(b), math.sin(b), math.cos(a) * math.cos(b))
        up = (-math.sin(a) * math.sin(b), math.cos(b), -math.cos(a) * math.sin(b))
        return right, up, back

    def fit(self, only=None):
        points = []
        for o in [only] if only else self.editor.doc["objects"]:
            b = self.bounds.get(o.get("asset"), (-0.5, -0.5, -0.5, 0.5, 0.5, 0.5))
            points.extend(maps.world(o, p) for p in itertools.product(*zip(b[:3], b[3:])))
        if points:
            self.center = [(min(c) + max(c)) / 2 for c in zip(*points)]
            aspect = max(0.2, self.winfo_width() / max(1, self.winfo_height()))
            self.distance = max(
                2, max(math.dist(p, self.center) for p in points) * 2.85 / min(aspect, 1)
            )
        self.redraw()

    def redraw(self):
        if self.pending is None:
            self.pending = self.after_idle(self.paint)

    def paint(self):
        self.pending = None
        if self.winfo_ismapped():
            self.render()

    def render(self, pick=False):
        w, h = max(1, self.winfo_width()), max(1, self.winfo_height())
        _, _, back = self.basis()
        eye = [c + self.distance * v for c, v in zip(self.center, back)]
        self.gl.vp_begin(w, h, V3(*eye), V3(*self.center), pick)
        objects = self.editor.doc["objects"]
        selected = None
        for idx, o in enumerate(objects):
            k = o["kind"]
            model = self.models.get(o.get("asset"))
            q = rotation.quaternion(rotation.orientation(o))
            self.gl.vp_object(
                idx,
                {"block": 0, "trigger": 1, "spawn": 2, "model": 3}[k],
                model,
                V3(*o["position"]),
                V4(*q),
                V3(*o["scale"]),
                o["id"] == self.editor.selection,
            )
            if o["id"] == self.editor.selection:
                selected = (idx, o, q)
        if not self.editor.pinning:
            for idx, o in enumerate(objects):
                for i, n in enumerate(o["notes"]):
                    self.gl.vp_pin(
                        idx, i, V3(*maps.world(o, n["local"])), n["id"] == self.editor.pin
                    )
            if selected:
                idx, o, q = selected
                self.gl.vp_gizmo(
                    idx,
                    self.tool,
                    V3(*o["position"]),
                    V4(*q),
                    self.session["axis"] if self.session else -1,
                )
        if not pick:
            self.gl.vp_end()

    def hit(self, x, y):
        self.render(True)
        hit = (I * 4)()
        point = V3()
        if self.gl.vp_hit(x, y, hit, point):
            return tuple(hit), list(point)
        return None, None

    def click(self, event):
        self.focus_set()
        if self.session:
            self.confirm()
            return
        e = self.editor
        if not e.finish_fields():
            return
        hit, point = self.hit(event.x, event.y)
        if e.pinning:
            if not hit or e.doc["objects"][hit[0]]["id"] != e.selection:
                e.status.set("Click a visible surface of the selected object.")
                return
            e.pinning = False
            e.pin = None
            e.notes.selection_clear(0, "end")
            e.note_fields(point)
            for label, ref in e.target_choices.items():
                if ref and ref["index"] == hit[1]:
                    e.target.set(label)
                    break
            e.original_note = e.note_input()
            e.logic.focus_set()
            self.redraw()
        elif hit:
            idx, _node, handle, note = hit
            o = e.doc["objects"][idx]
            if handle >= 0:
                self.start(self.tool, handle, (event.x, event.y), True)
            else:
                e.selection = o["id"]
                e.pin = o["notes"][note]["id"] if note >= 0 else None
                e.refresh()
        else:
            e.select(None)

    def orbit_start(self, event):
        if self.session:
            return
        self.focus_set()
        self.navigation = (event.x, event.y)

    def orbit(self, event):
        if not self.navigation:
            return
        x, y = self.navigation
        dx, dy = event.x - x, event.y - y
        self.navigation = (event.x, event.y)
        if event.state & 1:
            right, up, _ = self.basis()
            step = 2 * self.distance * math.tan(math.radians(24)) / max(1, self.winfo_height())
            self.center = [c + step * (-dx * r + dy * u) for c, r, u in zip(self.center, right, up)]
        else:
            self.azimuth -= dx * 0.009
            self.elevation = max(-1.55, min(1.55, self.elevation + dy * 0.009))
        self.redraw()

    def wheel(self, event):
        if not self.session:
            self.distance = min(
                100000, max(0.12, self.distance * (0.83 if event.delta > 0 else 1 / 0.83))
            )
            self.redraw()

    def choose(self, tool):
        self.cancel()
        self.tool = tool
        self.editor.active_tool.set(tool)
        self.focus_set()
        self.editor.status.set(TOOLS[tool] + " · Drag a colored handle, or press G / R / S.")
        self.redraw()

    def start(self, tool, axis=-1, mouse=None, drag=False):
        e = self.editor
        if not e.obj() or not e.finish_fields():
            return
        self.tool = tool
        self.editor.active_tool.set(tool)
        if mouse is None:
            mouse = (
                self.winfo_pointerx() - self.winfo_rootx(),
                self.winfo_pointery() - self.winfo_rooty(),
            )
        self.session = {
            "original": copy.deepcopy(e.obj()),
            "axis": axis,
            "start": mouse,
            "last": mouse,
            "number": "",
            "drag": drag,
            "valid": True,
        }
        self.evaluate()

    def evaluate(self):
        s = self.session
        original = s["original"]
        o = self.editor.obj()
        if o is None or o["id"] != original["id"]:
            self.cancel()
            return
        right, up, back = self.basis()
        depth = max(
            0.02,
            self.distance - dot([p - c for p, c in zip(original["position"], self.center)], back),
        )
        step = 2 * depth * math.tan(math.radians(24)) / max(1, self.winfo_height())
        dx = s["last"][0] - s["start"][0]
        dy = s["start"][1] - s["last"][1]
        axis = s["axis"]
        direction = rotation.AXES[axis] if axis >= 0 else back
        q = rotation.quaternion(rotation.orientation(original))
        guide = rotation.apply(q, direction) if self.tool == 2 and axis >= 0 else direction
        ax, ay = dot(guide, right), dot(guide, up)
        length2 = ax * ax + ay * ay
        along = (dx * ax + dy * ay) / length2 if length2 > 0.015 else dy
        typed = s["number"]
        s["valid"] = False
        try:
            n = float(typed) if typed else None
            if n is not None and (not math.isfinite(n) or abs(n) > 1e9):
                raise ValueError()
            if self.tool == 0:
                amount = n if typed else along * step
                delta = (
                    [amount * v for v in (direction if axis >= 0 else right)]
                    if axis >= 0 or typed
                    else [step * (dx * r + dy * u) for r, u in zip(right, up)]
                )
                if not typed and self.editor.snap.get():
                    delta = [round(v * 4) / 4 for v in delta]
                o["position"] = [p + v for p, v in zip(original["position"], delta)]
            elif self.tool == 1:
                # Angle around the projected pivot. Use a horizontal drag for edge-on rings.
                disp = [p - c for p, c in zip(original["position"], self.center)]
                cx = self.winfo_width() / 2 + dot(disp, right) / step
                cy = self.winfo_height() / 2 - dot(disp, up) / step

                def angle(v):
                    return math.atan2(cy - v[1], v[0] - cx)

                facing = dot(direction, back)
                degrees = math.degrees(angle(s["last"]) - angle(s["start"])) * (
                    1 if facing >= 0 else -1
                )
                if abs(facing) < 0.12 or math.dist(s["start"], (cx, cy)) < 8:
                    degrees = dx - dy
                amount = n if typed else degrees
                angles = rotation.angles(
                    rotation.multiply(rotation.axis_angle(direction, amount), q)
                )
                o.update(rotation=angles, yaw=angles[1])
            else:
                amount = n if typed else max(0.001, 1 + (along if axis >= 0 else dx + dy) / 100)
                if amount <= 0:
                    raise ValueError()
                o["scale"] = [
                    v * (amount if axis in (-1, i) else 1) for i, v in enumerate(original["scale"])
                ]
            readout = (
                (", ".join(f"{v:.3g}" for v in delta) + " m")
                if self.tool == 0
                else (f"{amount:.3g}°" if self.tool == 1 else f"{amount:.3g}×")
            )
            s["valid"] = True
        except (ValueError, OverflowError):
            readout = "Enter a valid value"
        label = "XYZ"[axis] if axis >= 0 else "Free"
        self.editor.status.set(
            f"{TOOLS[self.tool]} · {label}   {typed or readout}   |   X/Y/Z axis   ·   Enter confirms   ·   Esc cancels"
        )
        self.redraw()

    def motion(self, event):
        if self.session:
            self.session["last"] = (event.x, event.y)
            self.evaluate()

    def release(self, event):
        if self.session and self.session["drag"]:
            self.confirm()

    def confirm(self):
        if not self.session or not self.session["valid"]:
            return
        o = self.editor.obj()
        old = self.session["original"]
        self.session = None
        if o != old:
            updated = o.copy()
            o.clear()
            o.update(old)
            self.editor.checkpoint()
            o.update(updated)
        self.editor.refresh()
        self.editor.status.set(TOOLS[self.tool] + " confirmed.")

    def cancel(self):
        if self.session:
            old = self.session["original"]
            for o in self.editor.doc["objects"]:
                if o["id"] == old["id"]:
                    o.clear()
                    o.update(old)
                    break
            self.session = None
            self.editor.refresh()
            self.editor.status.set("Transform cancelled.")

    def key(self, event):
        key = event.keysym.lower()
        if self.session:
            s = self.session
            if event.state & 4:
                if key == "z":
                    self.cancel()
                return "break"
            if key == "escape":
                self.cancel()
            elif key in ("return", "kp_enter"):
                self.confirm()
            elif key in ("x", "y", "z"):
                axis = "xyz".index(key)
                s["axis"] = -1 if axis == s["axis"] else axis
                self.evaluate()
            elif key == "backspace":
                s["number"] = s["number"][:-1]
                self.evaluate()
            elif event.char and event.char in "-.0123456789":
                s["number"] += event.char
                self.evaluate()
            return "break"
        if event.state & 4:
            if key == "z":
                (self.editor.redo if event.state & 1 else self.editor.undo)()
                return "break"
            return
        if key in ("g", "r", "s"):
            self.start("grs".index(key))
            return "break"
        if key in ("f", "kp_decimal"):
            self.fit(self.editor.obj())
            return "break"
        if key == "delete":
            self.editor.remove()
            return "break"
