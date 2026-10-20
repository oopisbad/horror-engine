# Horror Engine — project notes

Custom 3D game engine (C++20, OpenGL 4.5 core) for a first-person psychological
survival horror game in the Cry of Fear style: near-black rooms, a flashlight
that is the only reliable light source, slow tense exploration.

Linux (CachyOS), project root `~/Projects`.

## Build & run

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/game
```

Dependencies are fetched by CMake (`FetchContent`): GLFW 3.4, GLM 1.0.1,
Dear ImGui v1.91.9b (assembled into a static lib from sources). GLAD 4.5 core
is checked in under `external/glad`; stb_image is under `external/stb`.

Shaders live in `shaders/` and are copied next to the executable by the
`copy_shaders` target on every build. The engine hot-reloads shaders by
polling file mtimes (editing either stage recompiles; a broken edit keeps the
previous working program).

### Debug CLI

```
./build/game [--w N] [--h N] [--vsync on|off] [--fullscreen]
             [--frames N] [--screenshot out.tga] [--shot-frame N] [--shadow N]
             [--ui title] [--debug-input] [--script "..."] [--selftest]
```

`--frames N` exits after N frames and `--screenshot` writes a TGA after ~30
frames, which is how rendering changes are smoke-tested headlessly.

Input debugging (added in the movement/mouse pass):

- `--debug-input` logs one line per fixed step (position, velocity, grounded,
  input axes, consumed mouse delta, yaw/pitch, collision push-out vector) plus
  one per frame (pending mouse delta, yaw/pitch).
- `--script "hold W 60; wait 30; mouse 120 0"` drives the real fixed step with
  synthetic input before the render loop. Commands: `hold <KEY[+KEY]> <steps>`,
  `release <KEY>`, `wait <steps>`, `mouse <dx> <dy>`. Keys: W A S D SPACE SHIFT
  CTRL. `hold` presses for exactly N steps, then releases.
- `--selftest` runs the scripted assertion matrix (walk W/A/S/D, diagonal,
  sprint, crouch, idle, jump, wall stop, mouse right/up/invert, key edges) on a
  private platform away from the corridor and exits non-zero on failure. It
  needs a GL window (Application owns the scene/camera), so it is scripted
  rather than fully headless.

### Controls

`WASD` move, mouse look (captured), `Shift` sprint, `Ctrl/C` crouch, `Space`
jump, `F1` overlay, `P` wireframe, `O` debug draw, `V` vsync, `Esc` quit.

Mouse convention: mouse right turns right, mouse up looks up, no acceleration
(GLFW raw mouse motion is enabled when the platform supports it). Look
sensitivity and an `invert Y` checkbox live in the F1 overlay; both are stored
on the `CharacterController` component (`mouseLookSensitivity`, `invertY`).
Camera yaw is counter-clockwise positive, so the controller *subtracts* the
horizontal delta.

## Architecture

```
engine/
  core/   Application (owns everything, main loop, hotkeys, screenshot)
          Window (GLFW + GL context, glad init, resize callbacks)
          Input (action map, edge-detected key/button state, mouse capture)
          Time (FrameTimer + FixedStepAccumulator)
          Log (std::format logger, asserts/backtraces)
          ResourceManager (generic weak-cached ResourceCache<T>)
  render/ Camera, Mesh, Shader, Texture, Material, Framebuffer,
          DebugDraw, Renderer (forward + shadows + post)
  scene/  Scene (components + transform hierarchy), Components,
          CharacterController (fixed-step character *system*)
  debug/  ImGuiOverlay
game/main.cpp   test scene construction + fixed-step wiring
shaders/        mesh (lit), shadow_depth, debug, post (fullscreen composite)
external/       glad (gl 4.5), stb
```

Deliberate choices worth knowing:

- **Application** owns Window/Input/Camera/Scene/Renderer and exposes
  `onFixedStep` / `onFrameUpdate` callbacks; gameplay lives in `game/`, never
  in the engine loop.
- **Scene** is a small hand-written ECS: dense entity ids, one `Transform`
  per entity, component stores are `unordered_map<EntityId, T>`. Entity ids
  are never recycled (destroy tombstones). Chosen over an archetype ECS
  because scenes have tens of entities; the API surface (`create`, `add`,
  `find`, `view`, `transform`) hides the storage.
- **Transforms** cache world matrices, recomputed every frame
  (`updateWorldMatrices`) with a depth cap to survive accidental cycles.
- **Rendering** is forward, single lit shader, uniforms per draw. The scene
  renders into an **HDR RGBA16F Framebuffer** with depth; a fullscreen
  triangle then applies fog (from depth), exposure, ACES tonemap, vignette,
  film grain and gamma. The shadow map (depth-only FBO) is produced by the
  flashlight spotlight with 2x2 PCF.
- **Meshes** are immutable after upload (interleaved VBO + EBO in one VAO,
  DSA setup). Procedural builders (cube/plane/sphere) are the only source of
  geometry so far.
- **Materials** are plain structs owned by the Scene pool; the renderer owns
  the uniform names. One shared lit shader draws everything.
- **Flashlight** = the first `SpotLight` component. Gameplay updates its
  entity transform (hand offset + camera yaw/pitch); the renderer reads the
  world matrix for the light origin/axis and builds the shadow camera from it.
- **Character controller** runs in the fixed step (1/60 s) so movement and
  collision are frame-rate independent. It is an AABB-vs-static-AABB,
  axis-separated resolver (X, Z, then Y), feet-origin, with head bob exposed
  on the Camera.
- **Input** maps semantic Actions to keys; per-key state keeps previous and
  current frame bits for pressed/held/released.

## Implemented

- GL 4.5 core context, windowed/fullscreen, vsync toggle, resize handling
  (framebuffer resize resizes the offscreen target and fixes the viewport;
  logical size updates the camera aspect).
- Fixed-timestep logic loop with accumulator, clamped frame delta and a
  max-steps cap against the death spiral.
- Action-mapped keyboard input + mouse capture, ImGui-aware input swallowing.
- Shader hot reload for all five programs.
- Forward renderer with per-frame render queue, bounding-sphere frustum
  culling, wireframe toggle, per-frame stats.
- Very low blue-tinted ambient, up to 8 point lights (inverse-square
  falloff), flashlight spotlight with 2x2-PCF shadow map, soft cone edge.
- HDR scene target + post pass: depth-based exponential fog, exposure, ACES
  tonemap, vignette, film grain, gamma. `post.enabled` can bypass to a blit.
- Camera: yaw/pitch, pitch clamp, head-bob offset/roll, roll-free basis for
  aim and the shadow camera.
- Scene: entities, transform hierarchy, AABB colliders with world bounds,
  material/mesh pools.
- Character controller: walk/sprint/crouch, gravity, jump, axis-separated
  AABB collision, crouch height smoothing, head bob.
- ImGui overlay: fps/frame time, draw stats, camera position, wireframe,
  debug draw, vsync, mouse capture toggles, post-process controls
  (enable/exposure/vignette/grain/fog) and ambient color/intensity.
- DebugDraw: lines, boxes, spheres (3 great circles), axes, frustums.
- Placeholder test scene: corridor + crates + ball, two point lights,
  flashlight, procedural meshes only.

## Partial / missing (target spec audit)

| Feature | State |
| --- | --- |
| Window/context, resize, vsync, fixed timestep | done |
| Input action mapping, mouse capture | done |
| Shader hot reload, Mesh, Material, forward queue | done |
| Camera first-person, pitch clamp, bob hook | done |
| Low ambient, point lights, flashlight spotlight + shadows | done (point lights do not cast shadows) |
| Framebuffer + post pass (vignette, grain, fog) | done |
| Simple ECS, transform hierarchy, AABB collision | done (AABB only, no capsule/slopes/step-offset) |
| Character controller walk/sprint/crouch/gravity | done |
| ImGui overlay (fps, frame time, camera pos, wireframe), debug draw | done |
| Placeholder cube/plane test scene | done |
| glTF 2.0 loading, skinned vertex format, animations | done (see Step 2 below) |
| Textures/normal maps in materials | partial: base-color textures are sampled; normal/metallic-roughness maps are loaded into `Material` but the forward shader does not sample them yet |
| Audio / UI / AI | intentionally absent (extension points only) |

## Step 2 — glTF / Blender readiness

- **Vertex format**: position, normal, uv0, tangent, `uvec4` joints, `vec4`
  weights (80 bytes, interleaved). Static meshes leave joints/weights zero.
  `mesh.vert` / `mesh.frag` and `shadow_depth.vert` are compiled twice, with
  and without `SKINNED`, so static and skinned draws share one pipeline.
- **GPU skinning**: 128-bone std140 UBO (8 KiB) uploaded per skinned draw; the
  same UBO is used by the flashlight shadow pass so shadows deform with the
  character.
- **Loader** (`engine/asset/GltfAsset`, cgltf 1.15 in `external/cgltf`):
  primitives, normals/tangent generation when absent, material factors and
  base-color/normal/metallic-roughness textures (buffer views, data URIs and
  external files), node hierarchy with matrix decomposition, skins with
  inverse bind matrices, animation samplers (linear/step/cubic-spline),
  `KHR_lights_punctual`, node `extras`. Cached through `AssetLibrary`.
- **Skinning space**: joint matrices are `inverse(meshNodeWorld) *
  jointWorld * inverseBind` per the glTF spec. The mesh node's world is the
  renderer's model matrix, so the two cancel; this matters for CesiumMan,
  which sits under Z-up conversion nodes.
- **Level conventions**: `COL_` (AABBCollider), `SPAWN_` (SpawnPoint),
  `TRIGGER_` (TriggerVolume), extras → EntityExtras, punctual lights →
  PointLight/SpotLight. Whole-level and prefab instantiation via
  `LevelBuilder::instantiate` (the returned `LevelInstance` owns the asset).
- **Animation**: `Animator` + `SkinnedMeshRenderer` components;
  `AnimationSystem::update` runs at the frame rate. Node-local TRS poses are
  sampled and blended (slerp for rotations), then node worlds and skin
  matrices are built. Crossfade via node-pose blending (not matrix blending),
  looping and one-shot clips, `finished` flag, time-based events from clip
  extras (`footsteps`).
- **Test content**: `assets/test/CesiumMan.glb` and `Fox.glb` load into the
  corridor. CesiumMan is meters; Fox is authored in centimeters and its level
  root uses a 0.012 scale. ImGui: per-animator clip combo, global playback
  speed, bone debug draw toggle.
- glTF is Y-up, right-handed, meters — the same as the engine, so no axis
  conversion; see `docs/BLENDER_EXPORT.md` for the export checklist.

### Step 2 known limitations

- Morph-target animation and `KHR_texture_transform` are not implemented.
- Skinned meshes are culled with their rest-pose bounds (fine for the sample
  models; large poses could pop at the screen edge).
- A skin bound to several mesh nodes with different transforms is only
  correct for the first mesh node (baked into `Skeleton::meshNodeInverse`).
- Multiple primitives each get their own Animator, so CPU evaluation is
  duplicated per primitive (Fox: 1 primitive; acceptable at this scale).
- Alpha modes other than OPAQUE are loaded as opaque; no transparency
  sorting.
- Normal mapping/metallic PBR shading is not in the forward shader yet; the
  data is loaded and ready.
- `COL_`/`TRIGGER_` bounds are computed at import time; they do not follow
  animated nodes.

## Bugs fixed in the stabilization pass

1. **Nothing was drawn** — `mainPass` skipped every item unless
   `material->shader` was set, but the material factory never set it and the
   renderer ignores it (one shared lit shader). The check is now only
   `material != nullptr`.
2. **Player launched out of the map** — the floor collider was authored in
   the rotated local space `(8, 0.05, 8)`, which the +90° X rotation turned
   into a 16 m tall, 0.1 m thick wall through the spawn. The floor now uses
   `-90°` about X (plane normal +Y instead of down) and local half-extents
   `(8, 8, 0.05)` so world bounds are a 16×16×0.1 slab at y≈0.
3. **Inside-out procedural meshes** — the cube's hand-written per-face u/v
   bases produced inward winding on 4 of 6 faces, and the sphere and plane
   index order wound inward as well. Back-face culling hid them. Faces now
   derive a consistent basis (`cross(u,v) == -n`) and sphere/plane indices
   are CCW outward.
4. **Right 20% of the screen never drawn** — `Framebuffer::bindDefault()`
   left the viewport at the 1024×1024 shadow-map size. The scene pass now
   binds its target and sets the viewport explicitly, and the post pass
   restores the swapchain viewport.
5. **Depth-only shadow FBO incomplete** — no `glDrawBuffer(GL_NONE)`; the
   inherited COLOR_ATTACHMENT0 draw buffer has no image. Added draw/read
   buffer = NONE plus a loud status log.
6. **Window resize would corrupt memory / crash** — `Window` stores `this`
   in GLFW's user pointer for its framebuffer callback, and `Input::attach`
   overwrote it with an `Input*` (then the callback wrote through it as a
   `Window*`). Input now resolves its instance through a static
   one-per-process pointer and leaves the user pointer alone.
7. **Camera aspect stale after resize** — `Window::width()/height()` were
   never refreshed; the framebuffer callback now also samples the logical
   window size.
8. **Point lights blew out the scene** — attenuation was `fade² * 4`, a
   constant 4× boost near the lamp with no inverse-square. Now
   `fade² / (d² + 0.5)`, and exposure/tonemapping moved to the post pass so
   the flashlight can be bright without clipping to white.
9. **`Input::released` returned on the first binding only** (loop body had an
   unconditional return) and crouch state `crouching` was never updated.
10. **Flashlight aim matrix** used `-X` for pitch, giving a mirrored vertical
    aim; gameplay now builds `yaw(+Y) * pitch(+X)`, matching
    `Camera::forward()`.
11. **The shadow map was always empty** — the post pass disables depth testing
    and the shadow pass inherited that state, so its depth-only FBO was
    cleared and never written (no shadows anywhere). The shadow pass now sets
    depth test/mask/cull state explicitly. Verified by reading the map back
    (44% coverage) and by tinting shadowed cone pixels (3.7% of the frame).
12. **Film grain was applied before gamma** — in the near-black scene the
    encode amplified the linear noise into TV static. Grain is now applied
    after the gamma encode.
13. **WASD made the player teleport** — the axis-separated resolver pushed out
    along the nearest face on the axis being moved, for *every* overlapping
    box. While grounded the body overlaps the floor slab on Y, so any X/Z
    motion also "collided" with the 24 m floor and the push-out moved the
    player to the slab edge (metres away). Resolution is now swept: a box can
    only block an axis if the body was outside that box's extent on that axis
    *before* the move. The Y push-out also had a feet-origin vs box-centre
    offset bug (it moved the feet up by half a body height per contact).
14. **Collision boxes were scale² too small** — `wall()`/`crate()` authored
    world-space half extents in `AABBCollider`, which is *local* space, and the
    entity's scale (sized for the unit cube) re-applied them: corridor walls
    ended up 4 cm thick and 10 m tall, crates half their visible size. The
    helpers now store the unit cube's local half (0.5); the transform supplies
    the world size. (Follow-up to bug 2.)
15. **Floor stopped short of the corridor end** — the slab was 16×16 and ended
    at z=-8 while the far wall sits at z=-10.5, so walking to the end dropped
    the player out of the level. Mesh + collider now share a 24×24 half-extent
    constant. (Second follow-up to bug 2.)
16. **Spawn was inside the floor** — the player entity started at y=0, 5 cm
    inside the slab, and the camera's authored (0,1.62,1.8) was overwritten by
    the controller on the first step. The feet spawn on the slab's top face
    (y=0.05) at z=1.8; the camera is derived from the feet.
17. **Mouselook was frame-rate dependent and yaw was inverted** — the fixed
    step read (never consumed) the per-frame delta, so a frame running two
    steps applied the same motion twice and a frame running none applied
    nothing. Deltas now accumulate in `Input` and are consumed exactly once per
    fixed step; `yaw += dx` (turn left on rightward motion) became
    `yaw -= dx`; raw mouse motion is enabled; free-cursor motion no longer
    turns the camera; sensitivity + invert-Y live in the F1 overlay. Vertical
    was already up-positive and was re-verified numerically and by screenshot
    (looking down shows the lit floor, looking up is black).
18. **Every key edge was dead** — `Input::update()` rolled `wasDown = isDown`
    *after* `glfwPollEvents`, i.e. it snapshotted the brand-new state, so
    `pressed()`/`released()` compared a key against itself and were always
    false. Jump, F1, P, O, V and Esc never fired. update() now runs at the top
    of the frame, before events (documented in the header).
19. **Colliders had zero bounds during the first fixed steps** —
    `refreshColliderBounds()` ran after the first gameplay steps, so the first
    steps tested degenerate boxes at the origin. Bounds are refreshed once
    before the loop.

## Known issues / TODO

- Point lights do not cast shadows (only the flashlight).
- No normal mapping yet; `Mesh` has no tangent attribute until step 2.
- AABB collision has no step-offset, slopes, or moving platforms; sub-step
  tunneling is only bounded by the fixed 1/60 s step (step-offset and sub-step
  protection are the next task).
- Collision resolution only blocks *entering* a face; a body spawned deeply
  inside geometry is not depenetrated (would need a push-to-nearest-face pass).
- `--selftest` needs a display because Application owns the window; a truly
  headless harness would need the scene/controller split out of Application.
- The post pass is a single composite; there is no bloom/SSAO chain yet.
- The imported test models read very dark in-game by design (ambient is
  near-black): point the flashlight at them, or raise ambient/post exposure in
  the F1 overlay when inspecting them.
- `Texture::placeholder()` is a function-local static destroyed at process
  exit, after the GL context is gone (harmless today, but should become an
  engine-lifetime owned object).
- Sound, UI/gameplay menus and AI are out of scope for now.
