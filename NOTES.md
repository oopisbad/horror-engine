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
             [--frames N] [--screenshot out.tga] [--shadow N] [--ui title]
```

`--frames N` exits after N frames and `--screenshot` writes a TGA after ~30
frames, which is how rendering changes are smoke-tested headlessly.

### Controls

`WASD` move, mouse look (captured), `Shift` sprint, `Ctrl/C` crouch, `Space`
jump, `F1` overlay, `P` wireframe, `O` debug draw, `V` vsync, `Esc` quit.

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
  debug draw, vsync, mouse capture toggles.
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
| glTF 2.0 loading, skinned vertex format, animations | missing (step 2) |
| Textures/normal maps in materials | partial (texture path exists, only base color, no loader) |
| Audio / UI / AI | intentionally absent (extension points only) |

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

## Known issues / TODO

- Point lights do not cast shadows (only the flashlight).
- No normal mapping yet; `Mesh` has no tangent attribute until step 2.
- AABB collision has no step-offset, slopes, or moving platforms; sub-step
  tunneling is only bounded by the fixed 1/60 s step.
- The post pass is a single composite; there is no bloom/SSAO chain yet.
- `Texture::placeholder()` is a function-local static destroyed at process
  exit, after the GL context is gone (harmless today, but should become an
  engine-lifetime owned object).
- Sound, UI/gameplay menus and AI are out of scope for now.
