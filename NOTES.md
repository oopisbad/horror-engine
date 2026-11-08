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
             [--test-models] [--debug-skin] [--debug-bones] [--patrol]
             [--wireframe]
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
- `--selftest` runs a 27-check scripted assertion matrix (walk W/A/S/D,
  diagonal, sprint, crouch, idle, jump, corridor wall stop, 0.2 m step-offset,
  6 cm thin-wall, mouse right/up/invert, key edges, landing bob continuity,
  red-ball trigger, mouse warp guard, resync counter, single-step look-spike
  rejection, flashlight beam aim, crouch eye height) on a private platform
  away from the corridor and exits non-zero on failure. It needs a GL window
  (Application owns the scene/camera), so it is scripted rather than fully
  headless. The
  last four checks are the sample models: the lowest vertex (rest pose and
  every sampled pose of the playing clip, 24 samples) must sit on `kFloorTopY`
  (0.05 m) within 1 mm, the foot/head vertex must stay within 0.30/0.60 m of
  the joint the overlay draws, and no joint may fall outside the skinned body.
  The selftest loads the sample models even without `--test-models`.
- `--test-models` loads `assets/test/CesiumMan.glb` and `Fox.glb` into the test
  scene. Default is off: the base scene is the procedural corridor + crates
  only. With the flag, the two models flank the corridor just ahead of the
  spawn (CesiumMan left at x=-1.0/z=-2.8 facing the player, Fox right at
  x=1.1/z=-2.4), inside the flashlight cone, clear of the crates and pillar,
  leaving the centre line walkable. They have no colliders.
- `--debug-skin` CPU-skins every vertex of the sample models with the exact
  joint matrices uploaded to the UBO and prints, for 9 times across the clip,
  the foot/head vertex, the joint position the skeleton overlay draws, the
  distance between them, the lowest/highest vertex, how many joints fall
  outside the skinned body, the distance from each joint to its nearest
  skinned vertex (`maxJointGap`, with the worst joint index), and the world
  AABBs of the skinned mesh and of the drawn joints. The gap is the direct
  "is the skeleton attached to the mesh?" number: a correct rig is a few
  centimetres, a wrong joint space is tens of centimetres or metres. It is
  O(joints x vertices), so it is only computed for this report and the
  selftest (`sampleSkin(..., measureJointGap=true)`). Poses are evaluated
  through `AnimationSystem::evaluate`, and the animator's playback state is
  restored afterwards. This is the tool that decides "is the mesh where we
  think it is" -- see bug 21. Implies `--test-models`.
- `--debug-bones` turns the skeleton overlay on at startup (same as the F1
  checkbox) so one run shows mesh + bones together. The bones are drawn in
  x-ray mode (depth test off) so the whole rig is visible through the body;
  a depth-tested skeleton hides inside the mesh and its visible fragments
  look detached from it.
- `--patrol` starts the sample models walking along their facing (same as `M`);
  their clips are in-place, so this is what makes them visibly walk.
- `--wireframe` starts with polygon mode LINE (same as pressing `P`), for
  geometry checks without touching the overlay.

### Flight recorder (`engine/debug/FlightRecorder`)

Always on, cheap, silent. Two ring buffers of the last ~420 records (~7 s):

- **per fixed step**: body position/velocity/grounded, collision push-out,
  camera yaw/pitch/position/eye height/bob offset/bob roll, input axes,
  consumed raw mouse delta and the look delta actually applied, capture /
  focus / ImGui state, condition flags (`LAND`, `JUMP`, `STEP`, `PUSH`,
  `PROBE`, `BOB`, `SUB`, `TELEPORT`, `LOOK_REJECTED`) and the tags naming the
  last system that wrote the camera and the player transform
  (`CharacterSystem`, `resetCharacter`, ...).
- **per frame**: delta, steps run, camera pose, pending mouse delta, capture /
  focus / ImGui state, which hotkeys fired (F1, P, O, V, Esc, F9, M) and the
  last camera writer.

**Automatic detection.** Each step compares the new record with the previous
one: yaw or pitch moving more than ~6 degrees beyond the applied mouse look,
a camera position moving more than `speed*dt + 6 cm + 0.30 m`, or the body
moving more than `speed*dt + 2 cm + 0.30 m`. On a hit it logs one `[recorder]`
WARNING with the offending numbers and writes `./debug_dump.txt` (the first
anomaly always dumps; repeats are rate limited to one every 0.5 s so a
per-step snap does not write a file per step). Deliberate resets call
`noteTeleport()`, so a spawn/respawn never looks like a bug -- those rows are
flagged `TELEPORT` instead.

The controller has a second, earlier tripwire: a single fixed step whose look
delta would exceed 0.35 rad (about ten times the fastest human flick) is
discarded before it is applied and reported through
`FlightRecorder::noteSpuriousInput`. That counts as an anomaly, logs
`[recorder] rejected look: ...` and dumps with a `rejected:` reason; the row
carries `LOOK_REJECTED`, with `look=(+0.0000,+0.0000)` and
`mouse=(+0.0,+0.0)` because the delta never reached the camera.

**F9 dumps on demand** to `./debug_dump.txt` (also `gitignore`d). Press it
immediately after a glitch. The header carries `reason:`, `steps=`, `frames=`,
`anomalies=`, `autoDumps=` and `capacity=`; rows are oldest first, all fixed
steps first, then frames.

Step row:

| column | meaning |
| --- | --- |
| `seq` | monotonic step sequence; it keeps counting across the ring wrap, so gaps are expected |
| `t` | cumulative seconds across the retained rows (the first row is its own dt) |
| `pos` | body (feet) world position |
| `vel` | body velocity, m/s |
| `g` | grounded (0/1) |
| `flags` | `LAND`, `JUMP`, `STEP`, `PUSH`, `PROBE`, `BOB`, `SUB`, `TELEPORT`, `LOOK_REJECTED` |
| `yaw`, `pitch` | camera angles after the step |
| `look` | yaw/pitch delta actually applied this step (after sensitivity and pitch clamp) |
| `mouse` | raw mouse pixels consumed this step, before sensitivity |
| `cam` | camera eye world position |
| `eye` | feet-to-eye height used for this camera position (crouch smoothing shows here) |
| `bob` | head-bob vertical offset and roll added to the eye |
| `push` | collision push-out applied this step |
| `cap`, `foc`, `gui` | mouse captured, window focused, ImGui wanted the mouse |
| `camWriter`, `playerWriter` | last system that wrote the camera / player transform |

Flag meanings: `LAND` touched down this step, `JUMP` impulse applied, `STEP`
walked up a ledge, `PUSH` collision push-out moved the body on X/Z, `PROBE`
the walk-off-edge probe changed `pos.y`, `BOB` head bob resumed, `SUB` more
than one collision pass, `TELEPORT` deliberate reset, `LOOK_REJECTED` the
step's look delta was discarded as a spike.

Frame row:

| column | meaning |
| --- | --- |
| `seq` | monotonic frame sequence number |
| `dt` | frame delta, seconds |
| `steps` | fixed steps run this frame (0 when the frame was fast, >1 on catch-up) |
| `cam`, `yaw`, `pitch` | camera pose after those steps |
| `pending` | mouse delta accumulated after the steps consumed theirs, i.e. what the next step will apply |
| `cap`, `foc` | capture/focus at the end of the frame |
| `guiMouse`, `guiKeys` | ImGui wanted the mouse / keyboard |
| `hotkeys` | hotkeys that fired this frame (`F1`, `P`, `O`, `V`, `Esc`, `F9`, `M`) |
| `camWriter` | last camera writer |

How to read it: `look=` is what the mouse actually applied; compare it with
the change in `yaw=`/`pitch=` between two rows to see a look jump, and check
`cap`/`foc`/`gui` on the rows around it -- a `cap` or `foc` flip is a
capture/focus transition, which is where camera snaps come from. A
`LOOK_REJECTED` row should show a zeroed `look`/`mouse` (the step was thrown
away). `TELEPORT` rows are deliberate resets, not bugs. `eye=` moving between
1.62 m standing and 0.97 m crouched is normal; an instant jump there is a
camera snap. Non-zero `push=` means collision moved the body, and
`camWriter`/`playerWriter` name whoever last wrote the camera/player
transform -- anything other than `CharacterSystem` on an anomaly row is
suspicious.

### Controls

`WASD` move, mouse look (captured), `Shift` sprint, `Ctrl/C` crouch, `Space`
jump, `F1` overlay, `P` wireframe, `O` debug draw, `V` vsync, `M` patrol the
sample models, `F9` flight recorder dump, `Esc` quit.

Mouse capture is re-baselined on every capture change *and* on every window
focus change: the cursor is warped by the window manager when the mode
changes, and the follow-up position event was hundreds of pixels of fake look
delta (the camera snap). The re-baseline clears the pending delta, swallows
three cursor events and then every sample for 8 more frames, because the WM
can move the pointer several frames later. The sample counter is guarded
(`if (resyncSamples_ > 0)` before the decrement), so extra cursor events leave
it at 0 instead of underflowing; the `resync counter` selftest feeds 40
samples plus 24 frames and then checks normal look still works.
`Input::cursorPosCallback` also drops any single event larger than 500 px as a
warp (warning the first 8 times, then only counting it in
`Input::warpEvents`).

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
  anim/   AnimationSystem (time, blending, skin matrices), Animator, Skeleton,
          SkinDebug (CPU-skinning probe for --debug-skin and the selftests)
  asset/  GltfAsset (cgltf loader), LevelBuilder (nodes -> entities)
  render/ Camera, Mesh, Shader, Texture, Material, Framebuffer,
          DebugDraw, Renderer (forward + shadows + post)
  scene/  Scene (components + transform hierarchy), Components,
          CharacterController (fixed-step character *system*)
  debug/  ImGuiOverlay, FlightRecorder (ring buffer + anomaly dumps)
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
- **Rendering** is forward, single lit shader, uniforms per draw. Materials
  sample base color (unit 3), normal map (unit 4) and metallic-roughness
  (unit 5); a missing map binds the renderer-owned 1x1 magenta placeholder
  with a `uHas*Tex` flag so one shader serves both cases. The scene
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
  entity transform (hand offset; beam aimed at the point the view ray hits
  12 m out, so it crosses the crosshair); the renderer reads the world matrix
  for the light origin/axis and builds the shadow camera from it.
- **Character controller** runs in the fixed step (1/60 s) so movement and
  collision are frame-rate independent. It is an AABB-vs-static-AABB,
  swept, axis-separated resolver (X, Z, then Y), feet-origin, with a 0.35 m
  step-offset (walk up curbs/ledges, headroom-checked) and collision
  sub-stepping capped at 8 cm per pass so fast movement cannot cross a
  collider between two overlap tests. Head bob is exposed on the Camera.
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
- Character controller: walk/sprint/crouch, gravity, jump, swept
  axis-separated AABB collision, 0.35 m step-offset, sub-stepped sweeps,
  crouch height smoothing (eye height follows it), head bob, per-step look
  spike rejection.
- Flight recorder: always-on step/frame ring buffers, automatic camera/body
  discontinuity detection with auto-dump, F9 on-demand dump.
- Skinned-model debug tooling: CPU-skinning probe (`--debug-skin`) with
  joint-to-mesh gap and AABBs, x-ray skeleton overlay (`--debug-bones`).
- Materials: base color (sRGB), tangent-space normal map and glTF
  metallic-roughness sampling in the forward shader; missing maps fall back to
  a renderer-owned magenta placeholder.
- ImGui overlay: fps/frame time, draw stats, camera position, mouse-look
  sensitivity slider + invert Y, wireframe, debug draw, vsync, mouse capture
  toggles, post-process controls (enable/exposure/vignette/grain/fog) and
  ambient color/intensity.
- DebugDraw: lines, boxes, spheres (3 great circles), axes, frustums; a
  depth-tested batch plus an x-ray overlay batch (used by the skeleton).
- Placeholder test scene: corridor + crates + ball, two point lights,
  flashlight, procedural meshes only (sample glTF models are opt-in via
  `--test-models`).

## Partial / missing (target spec audit)

| Feature | State |
| --- | --- |
| Window/context, resize, vsync, fixed timestep | done |
| Input action mapping, mouse capture | done |
| Shader hot reload, Mesh, Material, forward queue | done |
| Camera first-person, pitch clamp, bob hook | done |
| Low ambient, point lights, flashlight spotlight + shadows | done (point lights do not cast shadows) |
| Framebuffer + post pass (vignette, grain, fog) | done |
| Simple ECS, transform hierarchy, AABB collision | done (AABB + 0.35 m step-offset; no capsule, slopes or moving platforms) |
| Character controller walk/sprint/crouch/gravity | done |
| ImGui overlay (fps, frame time, camera pos, wireframe), debug draw | done |
| Placeholder cube/plane test scene | done |
| glTF 2.0 loading, skinned vertex format, animations | done (see Step 2 below) |
| Textures/normal maps in materials | done: base color (sRGB), tangent-space normal map, metallic-roughness (G=roughness, B=metallic); factors multiply the maps |
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
  corridor with `--test-models` (see the CLI section). CesiumMan is meters; Fox
  is authored in centimeters and its level root uses a 0.012 scale. ImGui:
  per-animator clip combo, global playback speed, bone debug draw toggle.
- **Placement is bounds-driven**: `spawnGltfLevel` instantiates the level, then
  `snapLevelToFloor` computes the world-space AABB of every rest-pose mesh
  (`levelWorldBounds`: transforms the mesh-local min/max corners by each mesh
  entity's `scene.worldMatrix`, so node hierarchy, Z-up conversion nodes, the
  root yaw and the 0.012 scale all apply) and adds `floorY - min.y` to the
  level ROOT's position. The spawn `y` passed to the helper is only a hint;
  feet land on the floor by construction. This is the fix for the sunk-model
  bug below.
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
- The forward shader is a cheap metal/roughness split on the existing
  Blinn-style model (metals lose diffuse and tint their highlight; the map
  drives roughness and metallic). It is not full GGX PBR and there is no IBL
  or AO map; the look stays tuned for a near-black flashlight game.
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

20. **CesiumMan and Fox were partly sunk into the floor** — `LevelBuilder`
    created each node entity with `scene.create(name, parent)` and then did
    `scene.transform(e) = transform`, assigning a whole `Transform{}`. That
    struct also owns `parent`/`children`, so the assignment reset the node's
    parent to `kInvalidEntity` and detached it from the level root while the
    root still listed it as a child. `updateWorldMatrices` then walked the
    node twice: once under the root and once as a root with an identity
    parent, which overwrote the world matrix. Result: the root's position,
    yaw and scale never reached the meshes — models rendered at their
    node-local origin (5 cm into the slab) and Fox, whose 79-unit height
    depends on the root's 0.012 scale, was not scaled at all. Fix: copy only
    position/rotation/scale into the existing transform (`setLocal` in
    `LevelBuilder::instantiate`). Root cause of the *symptom*: placement
    trusted the authored spawn height and the model origin. `spawnGltfLevel`
    now snaps the root from measured rest-pose world bounds, and the
    `--selftest` floor checks guard both models.

21. **The skeleton overlay did not line up with the skinned mesh** (bug 1).
    `Animator::jointWorldPositions` stored `nodeWorld[3]` -- the joint origin
    in *glTF scene space* -- and the debug draw multiplied it by the entity's
    world matrix (level root * mesh-node chain). The skinned vertices, on the
    other hand, go through `meshNodeInverse * jointWorld * inverseBind`, i.e.
    mesh-node space. The two only agree when the mesh node's world is identity.
    CesiumMan's mesh node sits under a Z-up conversion node and an armature
    node (both authored as `matrix` nodes), so the overlay drew a skeleton
    rotated ~90 degrees and displaced: measured with `--debug-skin`, the head
    joint was **1.85 m** from the head vertex, the foot joint dropped to
    y=-0.26 (under the floor), and **all 19 joints** were outside the skinned
    body. Fox was unaffected (its mesh node is a scene root with an identity
    transform) -- which is why only one model looked broken at a time. Fix: the
    joint position is now built in mesh-node space
    (`meshNodeInverse * jointWorld * origin`), the same space as the skin
    matrices, so multiplying by the entity world matrix puts it exactly where
    the vertices are. After the fix: 0 joints outside the body, foot/head
    offsets 0.03-0.15 m / 0.32-0.34 m (the ankle-to-sole and neck-to-crown
    distances, constant because they are rigid). `--debug-skin` also reports
    the joint-to-nearest-vertex gap, which puts a wrong joint space at tens of
    centimetres or metres and a correct rig at the centimetre scale, plus the
    world AABBs of the mesh and the joints; the selftest asserts the offsets
    and the joint count for both models. The overlay is also drawn in x-ray
    mode (`DebugDraw::setOverlayMode`): with depth testing on, most bones were
    hidden inside the mesh, so the visible fragments looked detached even when
    the joint space was correct.
22. **Feet sank through the floor during the walk clip** (bug 1, part 2).
    Floor placement used the rest-pose bounds, but the walk lowers the planted
    foot ~1-2.6 cm below the bind pose (hips bob, ankle rotates). Placement is
    now animation-aware: `levelLowestAnimatedY` CPU-skins 24 poses across the
    playing clip and lifts the root by the *lowest* one, so no sampled pose
    intersects the slab (verified per pose in the selftest, tolerance 1 mm).
23. **The camera flinched on landing** (bug 2). Not the ball: the head bob.
    `bobOffset` is damped to zero while airborne, and the old code drove the
    sine straight from it the instant `grounded` came back, so the strongest
    single-step discontinuity was up to `kBobVertical` (3 cm) of eye height
    plus roll -- at whatever phase the walk happened to resume. The amplitude
    is now ramped (`CharacterController::bobAmount`, 8/s) and the waveform is
    sampled with a continuous phase, so the per-step camera motion stays at
    walk-bob level: measured 15.4 mm (instant restart, old) vs 2.0 mm (ramp) on
    the landing step, 5.6 mm at worst over the whole jump. The "landing bob"
    selftest walks, jumps and lands at the red ball and asserts that the camera
    never moves more than 10 mm/step beyond what the feet do.
24. **The camera could snap to a fixed orientation** (bug 3). Mouse capture is
    a cursor-mode change, and the window manager warps the pointer; GLFW then
    delivers the *old* position and the warped position as two separate motion
    events. The old code swallowed only the first one, so the second arrived as
    a huge "delta" -- the recorded case was 205 px, i.e. 0.451 rad of yaw and
    0.453 rad of pitch, which pins pitch at its +/-89 degree clamp and reads as
    "the same fixed orientation" every time. A warp can also arrive several
    frames *after* the mode change (the WM moves the pointer asynchronously),
    which is why three swallowed samples alone were not enough. Focus
    loss/regain did the same thing (the pointer moves while unfocused and no
    re-baseline happened at all). Fixes: `Input` tracks window focus (focus
    callback + a per-frame `GLFW_FOCUSED` backstop) and re-baselines on both
    focus and capture transitions, clearing the pending delta, swallowing
    *three* samples and then every sample for the next 8 frames
    (`kResyncFrames`, ~130 ms at 60 fps); any single cursor event over 500 px
    is dropped as a warp and counted (`Input::warpEvents`). `firstMouseSample_`
    alone was exactly the bug. As a last line of defence `CharacterSystem`
    rejects any single fixed step whose look delta would exceed 0.35 rad (ten
    times the fastest human flick), zeroes it and reports the rejection to the
    flight recorder (`LOOK_REJECTED`, see the recorder section). The selftest
    drives the real cursor path (`Input::simulateCursorMove`) and asserts
    resync/warp deltas never reach the consumer while ordinary motion does;
    its `look spike` check feeds 900x900 px in one step and asserts no camera
    change, a counted rejection, and that the next normal delta still turns.
    The flight recorder flags any remaining unexplained yaw/pitch/body jump and
    dumps automatically. If it ever happens again: press **F9** and read
    `debug_dump.txt`.
25. **The red circle on the floor did nothing** (bug 2). It is the procedural
    `red-ball` entity: a 0.3 m UV sphere with albedo `(0.7, 0.1, 0.1)` resting
    on the slab at `(-0.8, 0.3, -1.5)`. It had **no collider and no trigger**, so
    it was a purely visual prop -- walking through it could not do anything,
    and the "flinch" seen while standing on it was the generic landing bob.
    It now carries a `TriggerVolume`; gameplay fires an edge-triggered enter
    event (`[trigger] entered 'red-ball' (1x)` in the console, `triggers:`
    block in the F1 overlay, blue box in the `O` debug draw that brightens
    while the player stands inside). The trigger is deliberately larger than
    the ball (0.30 m beyond the collider in X/Z, 0.60 m taller) so touching the
    side or standing on top both count, where a strict AABB test against the
    ball would not overlap.

    The ball is also solid now: an `AABBCollider` matching the sphere's mesh
    bounds (a 0.6 m cube centred at y=0.3, so its top is 0.55 m above the slab
    and above the 0.35 m step-offset -- you have to jump onto it). The
    `red-ball trigger` selftest walks into it and checks that the player stops
    at the near face (~z=-0.92) instead of passing through, fires exactly one
    enter event and is still inside.

26. **Crouching did not move the camera** -- the eye was a fixed 1.62 m above
    the feet, so crouching shrank the collider and slowed the walk but the
    view never came down (it read as "crouch does nothing"). The eye now
    scales with the already-smoothed body height (`eyeHeight *
    currentHeight / height`), so it follows standing 1.62 m down to ~0.97 m
    crouched and back without a snap. The `crouch eye` selftest asserts the
    standing/crouched/back heights and that no step moves the camera more than
    0.15 m.

27. **The flashlight's bright spot slid sideways near walls** -- the light
    origin keeps a hand offset (0.12 m right of and 0.10 m below the eye), but
    the old code aimed the beam *parallel* to the view. From an offset origin
    that puts the spot at `atan(offset / distance)` off the crosshair:
    invisible down a corridor, ~20 degrees on a wall 30 cm away.
    `syncFlashlight` now aims the beam at the point the view ray reaches at
    12 m, so it crosses the crosshair while the origin keeps the hand offset
    (0.156 m from the eye). The `flashlight aim` selftest asserts beam-vs-view
    under 2 degrees at three yaws plus the hand offset.

### Are the sample walk cycles supposed to stay in place?

Yes -- verified from the clip data, not by eye. Both clips animate only joint
rotations plus a small hip bob; there is no root translation channel that
would move the model through the world:

- Fox `Walk` (0.708 s): hip translation x in [-1.02, +1.23] cm, z in
  [40.05, 42.10] cm -- an oscillation around the bind pose, net travel 0.
- CesiumMan (2.0 s): torso joint z in [0.640, 0.711] m, y in [-0.030, -0.020]
  -- the stride bob, no forward motion.

The animator time does advance (the `--debug-skin` samples show different
vertex positions per time, and the lowest vertex moves through the cycle), so
"frozen in place" was the in-place clip, not a dropped channel. To make them
visibly walk, press `M` (or start with `--patrol`): gameplay translates each
level root +/-0.5 m along the model's own facing at 0.6 m/s (CesiumMan) and
0.45 m/s (Fox), turning around at the ends. Patrolling is gameplay moving a
transform; nothing about the skinning changes.

## Known issues / TODO

- Point lights do not cast shadows (only the flashlight).
- AABB collision has no slopes or moving platforms; the step-offset is flat
  and only applies while grounded. Sub-stepping caps movement at 8 cm per
  collision pass (8 passes max per fixed step).
- Collision resolution only blocks *entering* a face; a body spawned deeply
  inside geometry is not depenetrated (would need a push-to-nearest-face pass).
- `--selftest` needs a display because Application owns the window; a truly
  headless harness would need the scene/controller split out of Application.
- The post pass is a single composite; there is no bloom/SSAO chain yet.
- The imported test models read very dark in-game by design (ambient is
  near-black): point the flashlight at them, or raise ambient/post exposure in
  the F1 overlay when inspecting them. `--debug-bones` + `O` (debug draw) makes
  the skeleton and the red-ball trigger box visible without touching lighting.
- Screenshots are written vertically flipped: `takeScreenshot` reverses the
  GL rows into top-down order but leaves the TGA origin descriptor at 0
  (bottom-left). The pixel data is correct top-down, so viewing tools show it
  upside down; flip on load or set `header[17] = 0x20` to fix properly.
- Floor snapping now samples 24 poses of the clip *that is playing at spawn*.
  Switching to another clip afterwards (e.g. Fox `Run`) can dip below the slab
  again, because placement is not re-run per clip. Re-snap from `--debug-skin`
  numbers if a shorter/looser clip is added.
- Trigger volumes are static AABBs computed at setup; a trigger attached to a
  moving node (or a moving model) does not follow it.
- The mouse warp guard drops any single cursor event over 500 px. A genuine
  flick that large in one poll is not distinguishable from a warp; the event is
  dropped and counted (`Input::warpEvents`) rather than applied.
- Sound, UI/gameplay menus and AI are out of scope for now.
- The material placeholder is a `ForwardRenderer` member (no statics in
  `Texture`); any new GL-owning singleton must follow the same rule.

## Windows update — 2026-11-08

Windows is supported with the portable w64devkit C++20 compiler, CMake and Ninja.
Keep the extracted kit at tools/w64devkit inside this repository under Music.
No global PATH modification is required. Run build-windows.bat, then
run-windows.bat. Output is build-windows/game.exe; OpenGL 4.5 drivers are required.
After moving/renaming the repository, use a fresh CMake build directory: caches
contain absolute paths. The current installation lives in Music/goreX.
The Linux-only execinfo include is guarded, and ImGui explicitly links GLFW.

Controls: WASD movement; Shift sprint; Ctrl or C crouch; Space jump; F1 debug UI.
Camera sway/bounce and roll are substantially reduced; cadence is normalized to
walking speed and capped during sprinting. Space press events persist until a
physics step consumes them, including taps released within one event poll.
Jump requests buffer for 120 ms before landing, with 100 ms grace after leaving
an edge. A consumed press cannot repeatedly jump. Focus loss clears pending input.

Flashlight shadows now default to 2048 pixels with centered, weighted 5x5 PCF
instead of the offset 2x2 filter. This softens edges and reduces directional
filter artifacts. This remains shadow mapping, not ray tracing or area-light
contact-hardening; point lights still do not cast shadows. Use --shadow 1024
on slower hardware. Self-tests include render-only-frame jump taps and no repeat.
Assets and shaders are copied beside game.exe. Launch from its output directory when distributing the build.
Validation: Windows RelWithDebInfo build passed; 29/29 self-tests passed; 90-frame rendering test passed on Radeon RX 580 at 2048 shadow resolution.

## Main menu and player-hosted lobby — 2026-11-08

Normal launch opens a black menu with white Play, Settings and Multiplayer text
on the left. Settings is intentionally a placeholder pending the user's design.
Play starts solo; Escape pauses/resumes, with Resume and Main menu options.
Use Alt+F4/window close to exit. --play bypasses the menu for rendering tests.
Movement and animation stop in menus. Mouse capture and pending jump input reset
on transitions. F1 debug UI is available while playing.

GameSession separates session state, a tick counter, stable player IDs and local
scene entity handles. SessionPlayer.characterId is a placeholder selection key
for later character assets; supplied models are not imported yet.
MenuVideo is an adapter interface for opening a custom video, advancing playback,
and providing an OpenGL frame texture with an aspect ratio. Frontend::setVideo
accepts an adapter and file. The default remains black; no decoder or video file
is bundled, so arbitrary MP4 playback is not implemented yet.

Multiplayer is a working direct-IPv4 TCP lobby on port 27015, with one player
hosting and up to three guests. The host coordinates membership and ready state.
Join using the host's LAN IPv4; internet access requires a reachable host and
TCP port forwarding/firewall allowance. No automatic NAT traversal, relay,
matchmaking, authentication, encryption, or host migration is implemented.
Ready states are shared, but starting synchronized multiplayer gameplay is not
implemented: world replication and remote character control remain future work.
Host shutdown disconnects guests. Peers time out after 10 seconds without data;
connect attempts time out after 8 seconds. Protocol v1 has bounded buffers and
per-frame socket work, handles partial TCP reads/writes, and rejects a fifth peer.
The implementation uses Windows Winsock; tools remain under Music/goreX/tools.

Checks: --lobby-test exercises actual loopback TCP with 4 peers, ready propagation,
a fifth peer rejected, leave/rejoin, host shutdown and invalid address handling.
--frontend-test checks menu/play/pause/resume/leave/restart and mouse capture.
--selftest retains the movement/animation regression checks.
Validation: 29/29 gameplay checks, frontend lifecycle test, loopback lobby integration and menu screenshot inspection passed on Windows. No remote-machine or internet connectivity test has been performed.
Quit opens a centered "Misclick?" confirmation: Keep playing cancels, Quit game closes the application and lobby sockets. Escape dismisses the confirmation.
