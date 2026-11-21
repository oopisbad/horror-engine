# goreX Map Editor

Run `run-editor.bat` from the repository. The Windows editor uses Python 3 with Tkinter and a native OpenGL 3D viewport. The launcher calls the Windows build script if the viewport DLL is missing. Playtest uses the game built by `build-windows.bat`. You can also open
a project with `run-editor.bat "path\to\room.goremap"`.

Open `assets/maps/airlock.goremap` for a working example with the Import Lab and
an annotated airlock threshold.

## 3D viewport controls

Click a mesh or its entry in the object list to select it. Middle-drag orbits,
Shift-middle-drag pans, and the wheel zooms. Press **F** over the viewport to
frame the selection. **Fit map** frames the entire scene.

| Tool | Shortcut | Example |
| --- | --- | --- |
| Move | G | G, X, 2, Enter moves two metres along world X. |
| Rotate | R | R, Z, 90, Enter rotates 90 degrees around world Z. |
| Scale | S | S, Y, 0.5, Enter halves the object's local Y scale. |

Choose a tool in the toolbar to drag its colored handles. Red is X, green is Y,
blue is Z. With a keyboard transform active, move the mouse or type a value,
then left-click or press Enter to confirm. Escape or right-click cancels.
Tap the active axis again to release the constraint. Free move uses the view
plane, free rotation uses the viewing axis, and free scale is uniform.
Ctrl+Z undoes an edit. Ctrl+Shift+Z redoes it. The inspector accepts exact XYZ
positions, positive scales and Euler angles in degrees. The engine is Y-up.

## Light and dress a room

Turn on **Lit viewport** to see the game's materials, shadows, reflections and fog.
The editor draws on demand. The preview camera has no player flashlight or film grain.

- Choose **Lighting → Add spotlight**, press **G** to move it and **R** to aim its cone.
  In **Properties…** set its color, intensity, range and outer half-angle.
- **Add room fog** creates a box. Scale it to enclose a room, then set its density
  and color. Spotlights scatter inside the box and use the same shadow maps as the floor.
- Choose **Paint leak on surface** or **Paint chipped metal**, then click a mesh.
  The projector aligns to the face. Move, rotate or scale it with the usual tools.
  Opacity, tint and wetness are editable. Select the decal in the object list to
  edit it independently of the wall.
- Select a block or imported model and open **Properties…** to override roughness
  or normal-map strength for that placement. Leave a field empty to keep the
  source material value. Normal strength affects materials with normal maps.

The High preset handles up to four local spots, two shadowed spots, four visible
fog volumes and twelve decals. The Fast preset skips contact shading and fog.

## Build a room

1. Start with the floor and player spawn. Add a block, select it in the viewport
   or object list, then set its name, position, scale and rotation in the inspector.
   The block starts as a one-metre cube. Use its scale to size a wall or platform.
   A block exports visible geometry and a matching collision marker.
2. Use the move handles to place objects. Quarter-metre snapping applies to
   mouse translation. Typed distances use the exact value.
3. Use **Import model** for a local glTF 2.0 GLB or glTF scene. The imported
   scene moves as one placement. Solid mode shows material-colored meshes. Lit mode shows game lighting and textures. Playtest also animates characters.
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

Select the object that owns the behavior. Choose **Pin to 3D surface** and click
the desired spot on the selected mesh. The depth buffer supplies its world X/Y/Z. Adjust the coordinates, describe the behavior and the
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
