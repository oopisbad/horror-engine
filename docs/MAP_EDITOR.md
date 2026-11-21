# goreX Map Editor

Run `run-editor.bat` from the repository. The editor uses Python 3 with Tkinter.
Playtest uses the Windows game built by `build-windows.bat`. You can also open
a project with `run-editor.bat "path\to\room.goremap"`.

Open `assets/maps/airlock.goremap` for a working example with the Import Lab and
an annotated airlock threshold.

## Build a room

1. Start with the floor and player spawn. Add a block, select it in the plan
   or object list, then set its name, position, scale and yaw in the inspector.
   The block starts as a one-metre cube. Use its scale to size a wall or platform.
   A block exports visible geometry and a matching collision marker.
2. Drag objects in the X/Z plan. Turn off quarter-metre snapping for free
   placement. Use the inspector for exact X/Y/Z values, including height.
   Middle-drag pans, the wheel zooms at the pointer, and **Fit map** frames
   the objects. The list selects overlapping objects individually.
3. Use **Import model** for a local glTF 2.0 GLB or glTF scene. The imported
   scene moves as one placement. Its plan outlines come from mesh bounds.
   Import authored collision markers with the model, or add blockout collision
   using blocks. A trigger exports an invisible volume. The engine tests
   triggers using their world-axis-aligned bounding boxes.
4. Save the editable `.goremap` and choose **Playtest**. The editor generates
   a temporary level and opens the actual game. **File → Export playable glTF**
   writes a level you can launch with `run-windows.bat --level "room.gltf" --play`.

Projects reference source assets relative to the project when possible. Keep
their external textures, buffers and lightmaps with them. Use **Replace model
source** to repair a moved asset. Playable exports embed image and buffer data.
Distribute the adjacent `map-lightmaps` folder when the level uses baked maps.
The importer supports the engine's core glTF features, punctual lights,
emissive strength, unlit materials, texture transform metadata and mesh
quantization. It reports unsupported extensions at import. Export preserves
supported metadata, while shading follows the game's existing material loader.

## Describe logic for an AI agent

Select the object that owns the behavior. Choose **Place pin on plan** and click
the desired spot. Enter its exact world X/Y/Z, describe the behavior and the
condition that would demonstrate success, then click **Apply note**. A pin on
an imported mesh can identify its source node by name and glTF index. Use the
node selector when meshes overlap. Review node references after swapping or
re-exporting a source model.

For example, place a trigger just inside an airlock and pin its inner threshold.
A description could read “When the last living player crosses this threshold,
close both doors. Unlock the exit after both pressure switches are held for
three seconds. Re-entering the trigger must not restart the timer.”

The project keeps each pin in its object's local coordinates. Moving, scaling
or rotating the object carries the pin with it. **File → Export AI brief**
writes JSON with the map ID, stable object and note IDs, runtime node name,
placement transform, local pin coordinates, calculated world coordinates and
the requested behavior. Give the agent the brief, project and matching engine
revision. The brief records implementation requests. Trigger behavior still
requires game code.

Use **Edit → Undo** or **Redo** for applied object and note edits during the
session. Saving a project or exporting a brief applies pending inspector text.
