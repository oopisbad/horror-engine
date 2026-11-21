"""Exercise the compiled Windows viewport, picking and modal transform workflow."""

import math
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

import maps
import rotation

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(
    sys.platform == "win32" and (ROOT / "build-windows/gorex_viewport.dll").exists(),
    "Build the Windows viewport before running this test",
)
class ViewportTests(unittest.TestCase):
    def setUp(self):
        import tkinter as tk

        from map_editor import Editor

        self.root = tk.Tk()
        self.root.geometry("+10000+10000")
        self.e = Editor(self.root)
        self.root.update()
        self.v = self.e.canvas
        self.e.add("block")
        self.obj = self.e.obj()

    def tearDown(self):
        self.root.destroy()

    def command(self, *keys):
        for key in keys:
            self.v.key(SimpleNamespace(keysym=key, char=key if len(key) == 1 else "", state=0))

    def test_lit_lamp_and_painted_floor(self):
        import ctypes as ct

        from viewport import V3

        self.obj["position"][0] = 5
        self.e.add("spot")
        lamp = self.e.obj()
        lamp["rotation"] = [-90, 0, 0]
        lamp["position"] = [0, 3, 0]
        self.v.center = [0, 0, 0]
        self.v.distance = 8
        w, h = self.v.winfo_width(), self.v.winfo_height()
        eye = [self.v.distance * v for v in self.v.basis()[2]]

        def image():
            self.v.gl.vp_begin(w, h, V3(*eye), V3(*self.v.center), 0)
            self.v.draw_lit(w, h, eye)
            data = (ct.c_ubyte * (w * h * 4))()
            ct.windll.opengl32.glReadPixels(0, 0, w, h, 0x1908, 0x1401, data)
            return bytes(data)

        lit = image()
        signature = self.v.signature
        lamp["rotation"] = [90, 0, 0]
        unlit = image()
        self.assertIs(self.v.signature, signature)
        self.assertGreater(sum(lit[0::4]), sum(unlit[0::4]) + w * h)
        self.v.start_paint("chip")
        self.v.click(SimpleNamespace(x=w // 2, y=h // 2))
        decal = self.e.obj()
        self.assertEqual(decal["kind"], "chip")
        n = rotation.apply(rotation.quaternion(rotation.orientation(decal)), (0, 0, 1))
        self.assertGreater(n[1], 0.99)
        self.e.lit.set(True)
        self.v.render()
        self.assertTrue(self.e.lit.get(), self.e.status.get())

    def test_constraints_cancel_and_undo(self):
        self.command("g", "y", "-", "2", "Return")
        self.assertEqual(self.obj["position"], [0, -1.5, 0])
        self.command("r", "z", "9", "0", "Return")
        self.command("r", "x", "9", "0", "Return")
        p = maps.world(self.obj, [1, 0, 0])
        for a, b in zip(p, [0, -1.5, 1]):
            self.assertAlmostEqual(a, b)
        self.command("s", "y", ".", "5", "Return")
        self.assertEqual(self.obj["scale"], [1, 0.5, 1])
        self.command("g", "z", "5", "Escape")
        self.assertEqual(self.obj["position"], [0, -1.5, 0])
        self.e.undo()
        self.assertEqual(self.e.obj()["scale"], [1, 1, 1])
        for name, value in zip(("rx", "ry", "rz"), (10, 20, 30)):
            self.e.fields[name].delete(0, "end")
            self.e.fields[name].insert(0, str(value))
        self.assertTrue(self.e.apply())
        self.assertEqual(self.e.obj()["rotation"], [10, 20, 30])

    def test_pick_and_drag_handle(self):
        self.v.fit(self.obj)
        self.root.update()
        w, h = self.v.winfo_width(), self.v.winfo_height()
        # Project a point on the red handle into pixels.
        right, up, back = self.v.basis()
        length = self.v.distance * 64 / h
        t = length * 0.8
        factor = h / (2 * math.tan(math.radians(24)) * (self.v.distance - back[0] * t))
        x, y = round(w / 2 + right[0] * t * factor), round(h / 2 - up[0] * t * factor)
        event = SimpleNamespace(x=x, y=y)
        hit, _ = self.v.hit(x, y)
        self.assertEqual(hit[2], 0)
        self.v.click(event)
        self.v.motion(SimpleNamespace(x=x + 65, y=y))
        self.v.release(event)
        self.assertGreater(self.obj["position"][0], 0)
        self.e.start_pin()
        self.v.fit(self.obj)
        self.root.update()
        self.v.click(SimpleNamespace(x=w // 2, y=h // 2))
        self.assertFalse(self.e.pinning)
        self.e.logic.insert("1.0", "Activate when touched")
        self.assertTrue(self.e.apply_note())
        self.assertAlmostEqual(max(map(abs, self.obj["notes"][0]["local"])), 0.5, places=3)

    def test_legacy_import(self):
        self.e.saved = self.e.doc.copy()
        self.e.open(ROOT / "assets/maps/airlock.goremap")
        self.assertTrue(self.v.models)
        self.assertEqual(rotation.orientation(self.e.doc["objects"][0]), [0, 0, 0])


if __name__ == "__main__":
    unittest.main()
