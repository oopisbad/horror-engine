import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import gltf
import maps

ASSETS = Path(__file__).resolve().parents[1] / "assets" / "test"


class MapTests(unittest.TestCase):
    def test_round_trip_and_anchored_note(self):
        doc = maps.new_map()
        door = maps.item("trigger", "Airlock", (2, 1, 4), (2, 3, 4))
        note = {
            "id": maps.uid(),
            "local": [0.5, 1, 0.25],
            "logic": "Lock after entry. Unlock after both switches.",
        }
        door["notes"].append(note)
        door["yaw"] = 90
        doc["objects"].append(door)
        with TemporaryDirectory(dir=Path(__file__).resolve().parent) as tmp:
            project = Path(tmp) / "airlock.goremap"
            maps.save(project, doc)
            self.assertEqual(doc, maps.load(project))
            brief = project.with_suffix(".json")
            maps.brief(brief, doc)
            obj = json.loads(brief.read_text())["objects"][0]
            self.assertEqual(obj["notes"][0]["world"], [3, 4, 3])
            for result, expected in zip(maps.local(door, [3, 4, 3]), note["local"]):
                self.assertAlmostEqual(result, expected)
            level = project.with_suffix(".gltf")
            gltf.export(level, doc)
            names = [n["name"] for n in json.loads(level.read_text())["nodes"]]
            self.assertIn(obj["node"], names)
            self.assertIn("SPAWN_player", names)

    def test_lighting_export_and_material_isolation(self):
        doc = maps.new_map()
        floor = doc["objects"][0]
        floor["properties"] = {"roughness": 0.21}
        lamp = maps.item("spot", "Emergency")
        lamp["properties"] = {"intensity": 48, "color_g": 0.2}
        fog = maps.item("fog", "Hall")
        fog["properties"] = {"density": 0.4}
        chip = maps.item("chip", "Paint")
        doc["objects"].extend([lamp, fog, chip, maps.item("block", "Concrete")])
        with TemporaryDirectory(dir=Path(__file__).resolve().parent) as tmp:
            project = Path(tmp) / "map.goremap"
            maps.save(project, doc)
            level = project.with_suffix(".gltf")
            gltf.export(level, maps.load(project))
            out = json.loads(level.read_text())
            by_name = {n["name"]: n for n in out["nodes"]}
            self.assertEqual(by_name[maps.runtime_name(fog)]["extras"]["density"], 0.4)
            self.assertTrue(by_name[maps.runtime_name(lamp)]["extras"]["cast_shadow"])
            self.assertEqual(
                out["extensions"]["KHR_lights_punctual"]["lights"][0]["color"],
                [1, 0.2, 1],
            )
            self.assertIn("DECAL_CHIP_" + chip["id"], by_name)
            self.assertEqual(
                [m["pbrMetallicRoughness"]["roughnessFactor"] for m in out["materials"]],
                [0.8, 0.21],
            )

    def test_import_indices_and_dependencies(self):
        doc = maps.new_map()
        for filename in ("import-lab.glb", "CesiumMan.glb", "Fox.glb"):
            obj = maps.item("model", filename)
            asset = ASSETS / filename
            obj["asset"] = str(asset)
            doc["objects"].append(obj)
        with TemporaryDirectory(dir=Path(__file__).resolve().parent) as tmp:
            level = Path(tmp) / "test.gltf"
            gltf.export(level, doc)
            imported = json.loads(level.read_text())
            self.assertEqual(len(imported["skins"]), 2)
            self.assertTrue(imported["animations"])
            self.assertEqual(len(imported["extensions"]["KHR_lights_punctual"]["lights"]), 4)
            for node in imported["nodes"]:
                lightmap = node.get("extras", {}).get("lightmap")
                if lightmap:
                    self.assertTrue((level.parent / lightmap).exists())
            offset = len(imported["nodes"]) - len(gltf.read_asset(ASSETS / "Fox.glb")["nodes"]) - 1
            self.assertGreaterEqual(min(imported["skins"][-1]["joints"]), offset)
            self.assertTrue(
                all(i >= offset for n in imported["nodes"][offset:] for i in n.get("children", []))
            )


if __name__ == "__main__":
    unittest.main()
