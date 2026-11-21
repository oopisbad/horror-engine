"""Windows map authoring tool. Launch with run-editor.bat."""

import copy
import math
import struct
import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import gltf
import maps
from footprints import footprints

ROOT = Path(__file__).resolve().parents[1]
DOCUMENT_ERRORS = (
    OSError,
    ValueError,
    KeyError,
    TypeError,
    IndexError,
    AttributeError,
    struct.error,
    RecursionError,
)
COLORS = {"block": "#547486", "model": "#4c887e", "trigger": "#ac7446", "spawn": "#799854"}
UNIT = [(-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5)]


class Editor:
    def __init__(self, root):
        self.root = root
        self.doc = maps.new_map()
        self.path = None
        self.saved = copy.deepcopy(self.doc)
        self.history, self.redos = [], []
        self.selection = self.pin = self.drag = None
        self.pinning = False
        self.cache = {}
        self.zoom, self.center = 32.0, [0.0, 0.0]
        self.snap = tk.BooleanVar(value=True)
        self.status = tk.StringVar(
            value="Drag to move   ·   Middle drag to pan   ·   Wheel to zoom"
        )
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure(".", background="#222c32", foreground="#e2ebee", font=("Segoe UI", 10))
        style.configure(
            "TEntry", fieldbackground="#172026", foreground="#edf3f5", insertcolor="#edf3f5"
        )
        style.configure("TButton", background="#33424b", padding=(7, 4))
        style.map("TButton", background=[("active", "#405861"), ("pressed", "#48655f")])
        root.option_add("*Listbox.background", "#192227")
        root.option_add("*Listbox.foreground", "#d5e2e5")
        root.option_add("*Listbox.selectBackground", "#37695f")
        root.option_add("*Text.background", "#172026")
        root.option_add("*Text.foreground", "#edf3f5")
        root.option_add("*Text.insertBackground", "#edf3f5")
        root.geometry("1190x740")
        root.minsize(880, 540)
        menu = tk.Menu(root)
        for title, actions in [
            (
                "File",
                [
                    ("New map", self.new),
                    ("Open map…", self.open),
                    ("Save", self.save),
                    ("Save as…", lambda: self.save(True)),
                    ("Export playable glTF…", self.export),
                    ("Export AI brief…", self.brief),
                ],
            ),
            (
                "Edit",
                [("Undo", self.undo), ("Redo", self.redo), ("Delete selected object", self.remove)],
            ),
        ]:
            group = tk.Menu(menu, tearoff=False)
            for label, command in actions:
                group.add_command(label=label, command=command)
            menu.add_cascade(label=title, menu=group)
        root.config(menu=menu)
        bar = ttk.Frame(root, padding=6)
        bar.pack(fill="x")
        for label, command in [
            ("+ Block", lambda: self.add("block")),
            ("+ Trigger", lambda: self.add("trigger")),
            ("Player spawn", lambda: self.add("spawn")),
            ("Import model…", self.import_model),
            ("Fit map", self.fit),
            ("Playtest", self.play),
        ]:
            ttk.Button(bar, text=label, command=command).pack(side="left", padx=2)
        ttk.Checkbutton(bar, text="Snap 0.25 m", variable=self.snap).pack(side="right")
        body = ttk.Panedwindow(root, orient="horizontal")
        body.pack(fill="both", expand=True)
        left = ttk.Frame(body, padding=6)
        body.add(left, weight=0)
        ttk.Label(left, text="Map objects").pack(anchor="w")
        self.listing = tk.Listbox(left, width=21, exportselection=False)
        self.listing.pack(fill="both", expand=True, pady=5)
        self.listing.bind("<<ListboxSelect>>", self.pick_list)
        self.canvas = tk.Canvas(body, background="#151c21", highlightthickness=0)
        body.add(self.canvas, weight=1)
        side = ttk.Frame(body, padding=8)
        body.add(side, weight=0)
        ttk.Label(side, text="Object inspector").grid(row=0, column=0, columnspan=3, sticky="w")
        self.fields = {}
        for i, (label, keys) in enumerate(
            [
                ("Name", ["name"]),
                ("Position · X Y Z", ["x", "y", "z"]),
                ("Scale · X Y Z", ["sx", "sy", "sz"]),
                ("Yaw · degrees", ["yaw"]),
            ]
        ):
            ttk.Label(side, text=label).grid(
                row=2 * i + 1, column=0, columnspan=3, sticky="w", pady=(6, 0)
            )
            for j, k in enumerate(keys):
                field = ttk.Entry(side, width=9)
                field.grid(
                    row=2 * i + 2, column=j, columnspan=3 if len(keys) == 1 else 1, sticky="ew"
                )
                field.bind("<Return>", lambda e: self.apply())
                self.fields[k] = field
        ttk.Button(side, text="Apply transform", command=self.apply).grid(
            row=9, column=0, columnspan=3, sticky="ew", pady=6
        )
        ttk.Separator(side).grid(row=10, column=0, columnspan=3, sticky="ew", pady=4)
        ttk.Label(side, text="Logic notes").grid(row=11, column=0, columnspan=3, sticky="w")
        self.notes = tk.Listbox(side, height=5, width=30, exportselection=False)
        self.notes.grid(row=12, column=0, columnspan=3, sticky="ew")
        self.notes.bind("<<ListboxSelect>>", self.pick_note)
        ttk.Button(side, text="Place pin on plan", command=self.start_pin).grid(
            row=13, column=0, columnspan=3, sticky="ew", pady=4
        )
        ttk.Label(side, text="Pin world position · X Y Z").grid(
            row=14, column=0, columnspan=3, sticky="w"
        )
        self.coords = []
        for j in range(3):
            f = ttk.Entry(side, width=9)
            f.grid(row=15, column=j)
            self.coords.append(f)
        ttk.Label(side, text="Requested behavior / success condition").grid(
            row=16, column=0, columnspan=3, sticky="w", pady=(6, 0)
        )
        self.logic = tk.Text(side, width=30, height=7, wrap="word", undo=True)
        self.logic.grid(row=17, column=0, columnspan=3, sticky="ew")
        ttk.Button(side, text="Apply note", command=self.apply_note).grid(
            row=18, column=0, columnspan=2, sticky="ew"
        )
        ttk.Button(side, text="Delete", command=self.delete_note).grid(row=18, column=2)
        ttk.Label(side, text="Imported node reference").grid(
            row=19, column=0, columnspan=3, sticky="w", pady=(6, 0)
        )
        self.target = ttk.Combobox(side, state="readonly", width=25)
        self.target.grid(row=20, column=0, columnspan=3, sticky="ew")
        ttk.Button(side, text="Replace model source…", command=self.relink).grid(
            row=21, column=0, columnspan=3, sticky="ew", pady=4
        )
        ttk.Label(root, textvariable=self.status, padding=5).pack(fill="x")
        c = self.canvas
        c.bind("<Configure>", lambda e: self.draw())
        c.bind("<Button-1>", self.click)
        c.bind("<B1-Motion>", self.move)
        c.bind("<ButtonRelease-1>", lambda e: setattr(self, "drag", None))
        c.bind("<Button-2>", lambda e: setattr(self, "pan", (e.x, e.y, *self.center)))
        c.bind("<B2-Motion>", self.pan_view)
        c.bind("<MouseWheel>", self.wheel)
        root.bind("<Control-s>", lambda e: self.save())
        root.bind("<Escape>", lambda e: self.cancel_pin())
        root.protocol("WM_DELETE_WINDOW", lambda: root.destroy() if self.discard() else None)
        self.refresh()

    def obj(self):
        return next((o for o in self.doc["objects"] if o["id"] == self.selection), None)

    def note_input(self):
        return ([f.get() for f in self.coords], self.logic.get("1.0", "end-1c"), self.target.get())

    def finish_fields(self):
        if not self.obj():
            return True
        fields = [f.get() for f in self.fields.values()]
        note = self.note_input()
        write_note = note != self.original_note
        if write_note and not note[1].strip():
            self.error("Enter the logic description, or select Delete to remove an existing note.")
            return False
        if fields != self.original_fields:
            if not self.apply():
                return False
            if write_note:
                for f, v in zip(self.coords, note[0]):
                    f.delete(0, "end")
                    f.insert(0, v)
                self.logic.delete("1.0", "end")
                self.logic.insert("1.0", note[1])
                self.target.set(note[2])
        if write_note:
            return self.apply_note()
        return True

    def checkpoint(self):
        self.history.append(copy.deepcopy(self.doc))
        del self.history[:-80]
        self.redos.clear()

    def restore(self, source, dest):
        if source:
            dest.append(self.doc)
            self.doc = source.pop()
            self.pin = None
            self.refresh()

    def undo(self):
        self.restore(self.history, self.redos)

    def redo(self):
        self.restore(self.redos, self.history)

    def select(self, o):
        self.selection = o["id"] if o else None
        self.pin = None
        self.pinning = False
        self.refresh()

    def refresh(self):
        self.root.title(
            ("• " if self.doc != self.saved else "")
            + (self.path.name if self.path else "Untitled")
            + " | goreX Map Editor"
        )
        self.listing.delete(0, "end")
        for i, o in enumerate(self.doc["objects"]):
            self.listing.insert("end", o["kind"] + "   " + o["name"])
            if o["id"] == self.selection:
                self.listing.selection_set(i)
                self.listing.see(i)
        o = self.obj()
        values = [o["name"], *o["position"], *o["scale"], o["yaw"]] if o else [""] * 8
        for f, v in zip(self.fields.values(), values):
            f.delete(0, "end")
            f.insert(0, str(v))
        self.original_fields = [f.get() for f in self.fields.values()]
        self.notes.delete(0, "end")
        for i, n in enumerate(o["notes"] if o else []):
            self.notes.insert("end", n["logic"].splitlines()[0])
            if n["id"] == self.pin:
                self.notes.selection_set(i)
        self.note_fields()
        self.draw()

    def pick_list(self, event):
        indices = self.listing.curselection()
        if indices:
            oid = self.doc["objects"][indices[0]]
            if oid["id"] != self.selection and self.finish_fields():
                self.select(oid)

    def note_fields(self, point=None):
        o = self.obj()
        n = next((n for n in o["notes"] if n["id"] == self.pin), None) if o else None
        xyz = (
            point
            if point is not None
            else maps.world(o, n["local"])
            if n
            else o["position"]
            if o
            else [""] * 3
        )
        for f, v in zip(self.coords, xyz):
            f.delete(0, "end")
            f.insert(0, format(v, ".12g") if isinstance(v, (int, float)) else v)
        choices = {"Entire object": None}
        if o and o["kind"] == "model":
            for idx, name, _ in self.cache.get(o["asset"], []):
                choices[f"{idx} · {name}"] = {"index": idx, "name": name}
        self.target_choices = choices
        self.target.configure(values=list(choices))
        reference = n.get("source_node") if n else None
        self.target.set(
            next((label for label, value in choices.items() if value == reference), "Entire object")
        )
        self.logic.delete("1.0", "end")
        if n:
            self.logic.insert("1.0", n["logic"])
        self.original_note = self.note_input()

    def pick_note(self, event):
        indices = self.notes.curselection()
        if indices and self.obj():
            requested = self.obj()["notes"][indices[0]]["id"]
            if requested == self.pin or not self.finish_fields():
                return
            self.pin = requested
            self.note_fields()
            self.draw()

    def start_pin(self):
        if not self.finish_fields():
            return
        if self.obj():
            self.pinning = True
            self.status.set(
                "Click the plan for X/Z, set the exact world height, then apply the note."
            )
        else:
            self.status.set("Select the object that owns this note.")

    def cancel_pin(self):
        self.pinning = False
        self.status.set("Pin placement cancelled.")

    def apply_note(self):
        o = self.obj()
        if not o:
            return
        try:
            xyz = maps.vector([float(f.get()) for f in self.coords])
            description = self.logic.get("1.0", "end-1c").strip()
            if not description:
                raise ValueError("Describe the requested logic first.")
            anchor = maps.local(o, xyz)
            self.checkpoint()
            n = next((v for v in o["notes"] if v["id"] == self.pin), None)
            if n is None:
                n = {"id": maps.uid()}
                o["notes"].append(n)
            n.update(local=anchor, logic=description)
            target = self.target_choices.get(self.target.get())
            if target:
                n["source_node"] = target
            else:
                n.pop("source_node", None)
            self.pin = n["id"]
            self.pinning = False
            self.refresh()
            return True
        except ValueError as e:
            self.error(e)
            return False

    def delete_note(self):
        o = self.obj()
        if o and self.pin:
            self.checkpoint()
            o["notes"] = [n for n in o["notes"] if n["id"] != self.pin]
            self.pin = None
            self.refresh()

    def apply(self):
        o = self.obj()
        if not o:
            return
        try:
            vals = [f.get() for f in self.fields.values()]
            pos = maps.vector(list(map(float, vals[1:4])))
            scale = maps.vector(list(map(float, vals[4:7])))
            yaw = float(vals[7])
            if not vals[0].strip() or min(scale) <= 0 or not math.isfinite(yaw):
                raise ValueError("Enter a name, positive scale and finite angle.")
            self.checkpoint()
            o.update(name=vals[0].strip(), position=pos, scale=scale, yaw=yaw)
            self.refresh()
            return True
        except ValueError as e:
            self.error(e)
            return False

    def add(self, kind, asset=None):
        if not self.finish_fields():
            return
        if kind == "spawn":
            existing = next((o for o in self.doc["objects"] if o["kind"] == "spawn"), None)
            if existing:
                self.select(existing)
                return
        name = asset.stem if asset else kind.title()
        o = maps.item(kind, name, (self.center[0], 0.5 if kind == "block" else 0, self.center[1]))
        if asset:
            o["asset"] = str(asset)
        self.checkpoint()
        self.doc["objects"].append(o)
        self.select(o)

    def remove(self):
        o = self.obj()
        if o:
            self.checkpoint()
            self.doc["objects"].remove(o)
            self.select(None)

    def screen(self, x, z):
        return (
            (x - self.center[0]) * self.zoom + self.canvas.winfo_width() / 2,
            (z - self.center[1]) * self.zoom + self.canvas.winfo_height() / 2,
        )

    def plane(self, x, y):
        return (
            (x - self.canvas.winfo_width() / 2) / self.zoom + self.center[0],
            (y - self.canvas.winfo_height() / 2) / self.zoom + self.center[1],
        )

    def polygons(self, o):
        shapes = self.cache.get(o.get("asset"), []) if o["kind"] == "model" else []
        for _, _, poly in shapes or [(0, "", UNIT)]:
            pts = [maps.world(o, (x, 0, z)) for x, z in poly]
            yield [(p[0], p[2]) for p in pts]

    def fit(self):
        pts = [p for o in self.doc["objects"] for poly in self.polygons(o) for p in poly]
        if not pts:
            return
        xs, zs = zip(*pts)
        self.center = [(min(xs) + max(xs)) / 2, (min(zs) + max(zs)) / 2]
        self.zoom = max(
            0.1,
            min(
                200,
                (self.canvas.winfo_width() - 40) / max(1, max(xs) - min(xs)),
                (self.canvas.winfo_height() - 40) / max(1, max(zs) - min(zs)),
            ),
        )
        self.draw()

    def draw(self):
        c = self.canvas
        c.delete("all")
        w, h = c.winfo_width(), c.winfo_height()
        step = self.zoom
        while step < 18:
            step *= 5
        ox, oy = self.screen(0, 0)
        for x in range(round(ox % step), w, max(1, round(step))):
            c.create_line(x, 0, x, h, fill="#263239")
        for y in range(round(oy % step), h, max(1, round(step))):
            c.create_line(0, y, w, y, fill="#263239")
        for o in self.doc["objects"]:
            tag = "obj:" + o["id"]
            color = COLORS[o["kind"]]
            outline = "#e0eaf0" if o["id"] == self.selection else color
            shapes = self.cache.get(o.get("asset"), []) if o["kind"] == "model" else []
            for i, poly in enumerate(self.polygons(o)):
                tags = (tag, f"node:{shapes[i][0]} · {shapes[i][1]}") if shapes else (tag,)
                pts = [n for x, z in poly for n in self.screen(x, z)]
                c.create_polygon(*pts, fill=color, stipple="gray25", outline=outline, tags=tags)
            x, y = self.screen(o["position"][0], o["position"][2])
            c.create_text(x, y, text=o["name"], fill="#f1f4f5", tags=(tag,))
        for o in self.doc["objects"]:
            for n in o["notes"]:
                p = maps.world(o, n["local"])
                x, y = self.screen(p[0], p[2])
                c.create_oval(
                    x - 5,
                    y - 5,
                    x + 5,
                    y + 5,
                    fill="#ffd476",
                    outline="white" if n["id"] == self.pin else "#342d25",
                    tags=("note:" + n["id"], "obj:" + o["id"]),
                )
        c.create_text(8, 8, anchor="nw", fill="#adbec8", text="TOP   +X →   +Z ↓")

    def click(self, e):
        if not self.finish_fields():
            return
        x, z = self.plane(e.x, e.y)
        if self.pinning and self.obj():
            self.pinning = False
            self.pin = None
            self.notes.selection_clear(0, "end")
            self.note_fields([x, self.obj()["position"][1], z])
            for shape in reversed(self.canvas.find_overlapping(e.x, e.y, e.x, e.y)):
                tags = self.canvas.gettags(shape)
                if "obj:" + self.selection in tags:
                    node = next((t[5:] for t in tags if t.startswith("node:")), None)
                    if node:
                        self.target.set(node)
                        break
            self.original_note = self.note_input()
            self.logic.focus_set()
            return
        for shape in reversed(self.canvas.find_overlapping(e.x - 2, e.y - 2, e.x + 2, e.y + 2)):
            tags = self.canvas.gettags(shape)
            oid = next((t[4:] for t in tags if t.startswith("obj:")), None)
            if oid:
                self.selection = oid
                self.pin = next((t[5:] for t in tags if t.startswith("note:")), None)
                o = self.obj()
                if not self.pin:
                    self.drag = (x, z, list(o["position"]), False)
                self.refresh()
                return
        self.select(None)

    def move(self, e):
        if not self.drag or not self.obj():
            return
        x, z = self.plane(e.x, e.y)
        x0, z0, pos, moved = self.drag
        if not moved:
            if math.hypot(x - x0, z - z0) * self.zoom < 3:
                return
            self.checkpoint()
            self.drag = (x0, z0, pos, True)
        a, b = pos[0] + x - x0, pos[2] + z - z0
        if self.snap.get():
            a, b = round(a * 4) / 4, round(b * 4) / 4
        self.obj()["position"] = [a, pos[1], b]
        self.refresh()

    def pan_view(self, e):
        x, y, cx, cz = self.pan
        self.center = [cx + (x - e.x) / self.zoom, cz + (y - e.y) / self.zoom]
        self.draw()

    def wheel(self, e):
        a, b = self.plane(e.x, e.y)
        self.zoom = min(500, max(0.1, self.zoom * (1.2 if e.delta > 0 else 1 / 1.2)))
        x, z = self.plane(e.x, e.y)
        self.center = [self.center[0] + a - x, self.center[1] + b - z]
        self.draw()

    def error(self, error):
        messagebox.showerror("Map editor", str(error), parent=self.root)

    def import_model(self):
        name = filedialog.askopenfilename(filetypes=[("glTF model", "*.glb *.gltf")])
        if not name:
            return
        try:
            p = Path(name).resolve()
            self.cache[str(p)] = footprints(p)
            self.add("model", p)
        except DOCUMENT_ERRORS as e:
            self.error(e)

    def relink(self):
        obj = self.obj()
        if not obj or obj["kind"] != "model":
            self.status.set("Select an imported model first.")
            return
        if not self.finish_fields():
            return
        name = filedialog.askopenfilename(filetypes=[("glTF model", "*.glb *.gltf")])
        if not name:
            return
        try:
            path = Path(name).resolve()
            shapes = footprints(path)
            self.checkpoint()
            obj["asset"] = str(path)
            self.cache[str(path)] = shapes
            self.refresh()
            self.status.set("Model source replaced. Review any mesh node references on its notes.")
        except DOCUMENT_ERRORS as e:
            self.error(e)

    def discard(self):
        if self.doc == self.saved and (
            not self.obj()
            or (
                [f.get() for f in self.fields.values()] == self.original_fields
                and self.note_input() == self.original_note
            )
        ):
            return True
        choice = messagebox.askyesnocancel("Map editor", "Save changes to this map?")
        return self.save() if choice else choice is False

    def replace(self, doc, path):
        self.doc, self.path, self.saved = doc, path, copy.deepcopy(doc)
        self.history.clear()
        self.redos.clear()
        self.select(None)
        self.fit()

    def new(self):
        if self.discard():
            self.replace(maps.new_map(), None)

    def open(self, path=None):
        if not self.discard():
            return
        name = path or filedialog.askopenfilename(filetypes=[("goreX map", "*.goremap")])
        if not name:
            return
        try:
            path = Path(name).resolve()
            doc = maps.load(path)
            missing = []
            for asset in {o["asset"] for o in doc["objects"] if "asset" in o}:
                try:
                    self.cache[asset] = footprints(asset)
                except (OSError, ValueError, KeyError, IndexError) as e:
                    self.cache.pop(asset, None)
                    missing.append(str(e))
            self.replace(doc, path)
            if missing:
                self.status.set(
                    "Model unavailable. Select it and use Replace model source. " + missing[0]
                )
        except DOCUMENT_ERRORS as e:
            self.error(e)

    def save(self, as_new=False):
        if not self.finish_fields():
            return False
        name = (
            self.path
            if self.path and not as_new
            else filedialog.asksaveasfilename(
                defaultextension=".goremap", filetypes=[("goreX map", "*.goremap")]
            )
        )
        if not name:
            return False
        try:
            path = Path(name).resolve()
            maps.save(path, self.doc)
            self.path, self.saved = path, copy.deepcopy(self.doc)
            self.refresh()
            self.status.set("Saved " + str(path))
            return True
        except DOCUMENT_ERRORS as e:
            self.error(e)
            return False

    def export(self):
        if not self.finish_fields():
            return
        name = filedialog.asksaveasfilename(
            defaultextension=".gltf", filetypes=[("Playable glTF", "*.gltf")]
        )
        if not name:
            return
        try:
            target = Path(name).resolve()
            if any(
                Path(o["asset"]).resolve() == target for o in self.doc["objects"] if "asset" in o
            ):
                raise ValueError("Choose an export filename separate from the imported model.")
            gltf.export(target, self.doc)
            self.status.set("Exported " + str(target))
        except DOCUMENT_ERRORS as e:
            self.error(e)

    def brief(self):
        if not self.finish_fields():
            return
        name = filedialog.asksaveasfilename(
            defaultextension=".json", filetypes=[("AI logic brief", "*.json")]
        )
        if name:
            try:
                maps.brief(Path(name), self.doc)
                self.status.set("Exported " + name)
            except DOCUMENT_ERRORS as e:
                self.error(e)

    def play(self):
        if not self.finish_fields():
            return
        try:
            exe = ROOT / "build-windows" / "game.exe"
            if not exe.exists():
                raise ValueError("Build the game with build-windows.bat before playtesting.")
            folder = exe.parent / "editor-preview"
            folder.mkdir(exist_ok=True)
            target = folder / (self.doc["id"] + ".gltf")
            gltf.export(target, self.doc)
            subprocess.Popen(
                [str(exe), "--level", str(target), "--play", "--fps-cap", "60"], cwd=ROOT
            )
            self.status.set("Playtest running in the game window.")
        except DOCUMENT_ERRORS as e:
            self.error(e)


if __name__ == "__main__":
    root = tk.Tk()
    editor = Editor(root)
    if len(sys.argv) > 1:
        root.after_idle(lambda: editor.open(sys.argv[1]))
    root.mainloop()
