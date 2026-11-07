# Horror Engine — bug review

Scope: every hand-written source file under `engine/` and `game/` plus the GLSL
under `shaders/` (third-party code in `external/` was skipped). Findings were
confirmed by reading the code; nothing here is a guess or a style preference.
Items already listed under "Known issues / TODO" in `NOTES.md` (upside-down
TGA screenshots, one skin shared by several nodes, alpha modes, etc.) are
deliberately not repeated.

Sections 1-10 come from the first read-through, 11-18 from a second pass over
the same files plus the GLSL, and 19-25 from a third pass over `game/main.cpp`,
the controller/camera path, `Shader` hot reload and the level-import options.
The numbering is chronological by pass and each pass is ordered by severity
within itself, so a low from pass 1 can precede a medium from pass 2; it is not
a single global ranking.

Severity: **high** = memory unsafety / crashes, **medium** = wrong behaviour
that shows up in normal play or memory unsafety reachable from ordinary input
(assets, gameplay calls), **low** = latent or diagnostic defects.

## Resolution status (branch `bug-review-notes`, 2026-11-07)

All 25 findings are fixed. The table below is the original review, kept as the
problem statement; `NOTES.md` records the change made for each number. The
fixes are on `bug-review-notes`, not on `master`.

What was verified locally (CachyOS, Intel UHD 600; no RX 580 or RTX 5070 in
this machine, no Windows runtime):

* `--selftest` passes **35/35** on a clean host build, including new checks for
the previously latent defects: crouch headroom (14), `destroy()` child
  re-homing (3), `view()` mutation during iteration (12), timestep backlog (4),
  loop-crossing animation events (11), trigger world bounds (5), and a
  single-application bob check that reconstructs the eye from the view matrix
  (19/20).
* The same self-test and a `--screenshot` at 1281x720 (a width the old
  readback overflowed) run clean under AddressSanitizer + UndefinedBehaviorSanitizer;
  the TGA is exactly `1281*720*3 + 18` bytes.
* A `-DNDEBUG` program confirms `HOR_ASSERT` is skipped and `HOR_CHECK` aborts
  (finding 2).
* Shader hot reload was exercised for real: broke `post.frag`, restored it
  without changing its mtime, and observed the throttled retry log
  `shader reloaded [post]` (finding 22).
* Linux package extracted outside the repo and launched from its own directory;
  `assets/` and `shaders/` resolve relative to the working directory.
* Windows x86-64 release cross-compiled with llvm-mingw 2026-11-04 (UCRT,
  `-static` libc++/libunwind): 3.9 MB PE32+ console exe. Imports are only the
  Windows-provided UCRT `api-ms-win-crt-*` plus KERNEL32, USER32, GDI32 and
  SHELL32; GLFW is static and GLAD loads `opengl32.dll` at runtime. The zip was
  re-extracted and its exe hash matches the build output.
* Two build-system defects surfaced only when cross-compiling: imgui's GLFW
  backend was silently using a system GLFW header (now `imgui` links `glfw`
  explicitly), and the toolchain file now defaults `MINGW_ROOT` so CMake's
  `try_compile` children, which do not inherit `-D`/environment, can find the
  compiler.

Not verified: the Windows executable has never been run (no Windows host or
Wine here), and neither GPU was available. RX 580 on Windows 11 is the primary
release target; AMD lists OpenGL 4.5 for the card, exactly the engine's core
requirement, but real Adrenalin behaviour, shader compilation and VRAM use on
that hardware are still untested.

Release: tag `v0.1.0-beta` attaches
`horror-engine-0.1.0-windows-x86_64.zip` (Windows, RX 580 / Windows 11 focus)
and `horror-engine-0.1.0-linux-x86_64.tar.gz`. The repository is public at
https://github.com/oopisbad/horror-engine and the release (marked pre-release)
is at https://github.com/oopisbad/horror-engine/releases/tag/v0.1.0-beta;
both archives were downloaded anonymously from that URL and their SHA-256
hashes match the local build outputs.

| # | Severity | Where | Bug |
| - | -------- | ----- | --- |
| 1 | high | `engine/core/Application.cpp:168-169` | Screenshot readback ignores `GL_PACK_ALIGNMENT` → heap overflow |
| 2 | medium | `engine/core/Log.hpp:59-63` | `HOR_CHECK` is compiled out in release/RelWithDebInfo |
| 3 | medium | `engine/scene/Scene.cpp:45-67` | `destroy()` orphans children; their world matrices freeze forever |
| 4 | medium | `engine/core/Time.hpp:46-56` | Fixed-step accumulator never drops its backlog → unbounded debt |
| 5 | medium | `engine/asset/LevelBuilder.cpp:151-180` | `TRIGGER_`/`SPAWN_` data ignores the level root transform |
| 6 | medium | `engine/render/Texture.cpp:27-30` | `Params::srgb` is dead → base-colour maps are never sRGB-decoded |
| 7 | low | `engine/scene/Scene.cpp:75-84` | `attachTo()` accepts cycles and unvalidated ids |
| 8 | low | `engine/render/Renderer.cpp:473-474` | Post pass hardcodes the camera near/far planes |
| 9 | low | `engine/core/Window.cpp:127-130` | `resizedSinceLastFrame` can never be observed `true` |
| 10 | low | `engine/asset/GltfAsset.cpp:299-311` | `decodeBase64()` shifts a signed `int` until it overflows (UB) |
| 11 | medium | `engine/anim/AnimationSystem.cpp:293` | Looping clips drop every animation event that crosses the loop point |
| 12 | medium | `engine/scene/Scene.hpp:69-76` | `view()` iterates the store it hands out; a callback that despawns UB-invalidates it |
| 13 | medium | `engine/asset/GltfAsset.cpp:620-631` | glTF indices are never range-checked → OOB read/write in normal generation |
| 14 | low | `engine/scene/CharacterController.cpp:319-321` | Standing up from a crouch ignores headroom and clips into ceilings |
| 15 | low | `engine/render/Renderer.cpp:380` | A mesh with no material casts a shadow but is never drawn |
| 16 | low | `engine/render/Renderer.cpp:117-125` | `tuneFlashlight()` is overwritten by `render()` every frame |
| 17 | low | `engine/asset/GltfAsset.cpp:460-494` | Texture cache ignores the per-use sRGB flag |
| 18 | low | `engine/core/Input.cpp:259-268` | `released()` bypasses the ImGui keyboard gate that `held()`/`pressed()` honour |
| 19 | medium | `engine/scene/CharacterController.cpp:366-367` | Head-bob is folded into `Camera::position` and added again by `Camera::update` |
| 20 | low | `engine/scene/CharacterController.cpp:347` | Bob sway is applied along world +X instead of camera-right |
| 21 | low | `engine/render/Renderer.cpp:265-286` | The flashlight is whichever `SpotLight` the store visits last |
| 22 | low | `engine/render/Shader.cpp:237-239` | A failed hot reload records the new mtime, so it is never retried |
| 23 | low | `engine/scene/CharacterController.cpp:271-273` | The sub-step clamp can exceed its own 8 cm bound on long falls |
| 24 | low | `engine/asset/LevelBuilder.cpp:85-95` | Skipped marker nodes lose their transform for their children |
| 25 | low | `engine/asset/LevelBuilder.cpp:160-180` | `TRIGGER_` without a mesh creates a zero-size volume at the world origin |

---

## 1. Screenshot readback overflows the pixel buffer (high)

**File:** `engine/core/Application.cpp:152-196` (the readback itself is lines
168-169)

```cpp
std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);   // 168
glReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data()); // 169
```

**What is wrong:** `glReadPixels` honours `GL_PACK_ALIGNMENT`, which defaults to
4. Each row in client memory therefore starts at a 4-byte boundary, so a
readback of `w` RGB pixels occupies `align4(3*w)` bytes per row, not `3*w`.
The buffer allocated here is exactly `w*h*3` bytes, so for any window width
where `3*w % 4 != 0` (e.g. 1281, 1366, 1601 — any width that is not a multiple
of 4) the driver writes up to `3*h` bytes past the end of the vector. The
driver is entitled to do that: the spec makes it the caller's responsibility to
supply a buffer sized for the *aligned* rows.

Two consequences:

* heap corruption in `--screenshot` runs (the buffer is a `std::vector<uint8_t>`
  on the heap);
* even when it happens not to fault, the manual flip loop just below
  (`pixels[(h-1-row) * w * 3]`) assumes tightly packed rows, so the image is
  sheared/corrupted rather than merely padded.

Widths that are multiples of 4 (1280, 1920, 2560) hide the bug, which is why it
survives casual smoke tests.

**Suggested fix:** set the pack alignment before reading and restore it after,
and size the buffer defensively:

```cpp
glPixelStorei(GL_PACK_ALIGNMENT, 1);
glReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
glPixelStorei(GL_PACK_ALIGNMENT, 4);
```

(Also worth checking `glGetError()`/`glReadPixels`' result, and guarding against
`w <= 0 || h <= 0` before allocating.)

---

## 2. `HOR_CHECK` does nothing in release builds (medium)

**File:** `engine/core/Log.hpp:59-66`

```cpp
// HOR_ASSERT: active in debug builds only (hot paths, cheap sanity checks).
// HOR_CHECK:  active in all builds -- use for engine-integrity conditions.
#if defined(NDEBUG)
#define HOR_ASSERT(expr) ((void)(expr))
#define HOR_CHECK(expr)  ((void)(expr))
...
```

**What is wrong:** despite the documented contract, `HOR_CHECK` expands to
`((void)(expr))` when `NDEBUG` is defined, i.e. it evaluates the expression and
throws the result away. `CMakeLists.txt:7-9` defaults the build type to
`RelWithDebInfo`, which defines `NDEBUG`, so the *default configuration*
disables every "engine-integrity" check in the codebase. The call sites are not
cosmetic — they are the only guards protecting several unsafe operations:

* `engine/scene/Scene.hpp:88` — `worldMatrix(e)` only `HOR_CHECK`s
  `e < worldMatrices_.size()`; in release a stale/foreign id is an out-of-bounds
  `std::vector` read (`Renderer::buildQueue`, `main.cpp` helpers all index it).
* `engine/render/DebugDraw.cpp:157-158` — `HOR_CHECK(ptr != nullptr)` followed by
  `std::memcpy(ptr, ...)`; if the buffer map fails in release, this memcpy hits
  a null pointer instead of failing loudly.
* `engine/render/Framebuffer.cpp:116` — an incomplete FBO only logs; the
  `HOR_CHECK` that was meant to stop the run does nothing.
* `engine/render/Mesh.cpp:119`, `engine/render/Texture.cpp:123` — binding a
  moved-from/zeroed GL object silently becomes a no-op GL call.

Because the macro evaluates `expr`, the check *looks* active in code review, so
this is easy to miss.

**Suggested fix:** make `HOR_CHECK` unconditional (log + `std::abort()` via
`detail::assertFail`, in both debug and release), and keep only `HOR_ASSERT`
compiled out:

```cpp
#if defined(NDEBUG)
#define HOR_ASSERT(expr) ((void)0)          // hot-path sanity only
#else
#define HOR_ASSERT(expr) ((expr) ? (void)0 : ...)
#endif
#define HOR_CHECK(expr)  ((expr) ? (void)0 : ::horror::detail::assertFail(__FILE__, __LINE__, #expr))
```

If aborting in release is considered too harsh, the alternative is to make
`HOR_CHECK` log an error and return a safe value at each of the four call sites
above — but the current silent no-op is the worst of both worlds.

---

## 3. `Scene::destroy()` leaves children parented to a tombstone (medium)

**File:** `engine/scene/Scene.cpp:45-67`, interacting with
`Scene::updateWorldMatrices()` at `engine/scene/Scene.cpp:88-110`

```cpp
void Scene::destroy(EntityId e)
{
    if (!valid(e)) return;
    entityRecords_[e].alive = false;
    EntityId p = transforms_[e].parent;
    if (p != kInvalidEntity) { std::erase(transforms_[p].children, e); transforms_[e].parent = kInvalidEntity; }
    /* components erased ... */
}
```

**What is wrong:** `destroy()` detaches the destroyed entity from its parent but
never touches its *own* `children` list, and never rewrites the children's
`parent` field. Those children now point at a dead entity. Since
`updateWorldMatrices()` only starts a walk at entities whose
`transforms_[i].parent == kInvalidEntity` (line 107) and skips dead roots, a
child of a destroyed entity is never visited again:

* its `worldMatrices_[e]` stays frozen at whatever it was on the frame of the
  `destroy()` call;
* later edits to that child's `Transform` (position/rotation/scale) have no
  visible effect — the renderer keeps using the stale matrix from
  `Scene::worldMatrix()`;
* `refreshColliderBounds()` keeps deriving collider bounds from the frozen
  matrix, so collision stays at the old place.

In the current code `destroy()` is only used on leaf entities (the `--selftest`
pad/ledge/thin-wall), so this is latent, but it is a landmine for the first
gameplay system that despawns a prop with attached meshes.

**Suggested fix:** reparent the children (or at least detach them so they become
roots again) inside `destroy()`:

```cpp
for (EntityId child : transforms_[e].children) {
    transforms_[child].parent = kInvalidEntity;   // or the destroyed entity's parent
    if (p != kInvalidEntity) transforms_[p].children.push_back(child);
}
transforms_[e].children.clear();
```

---

## 4. Fixed-step accumulator never discards its backlog (medium)

**File:** `engine/core/Time.hpp:43-68`

```cpp
int stepsToRun(float frameDelta) {
    leftover_ += frameDelta;
    int steps = 0;
    while (leftover_ >= step_ && steps < maxStepsPerFrame_) { leftover_ -= step_; ++steps; }
    return steps;
}
```

**What is wrong:** the per-frame cap (`maxStepsPerFrame_ = 8`) limits how much
simulation runs, but the time that was *not* simulated stays in `leftover_`
forever. `FrameTimer` clamps a single frame delta to 0.25 s (15 steps), so a
stalled/slow frame can add up to `0.25 - 8/60 ≈ 0.117 s` of permanent debt that
is never forgiven. On a machine that sits below ~7.5 fps (software GL, debugger,
heavy scene) *every* frame adds debt: `leftover_` grows without bound, gameplay
runs at 8/60 s of simulation per frame instead of real time, and
`alphaFraction()` (documented as `[0,1)`) returns values far above 1. The
comment claims this cap "avoids the death spiral", but without dropping the
excess it merely converts a spiral of ever-longer frames into a permanent
slow-motion backlog.

**Suggested fix:** when the cap is hit, drop the remaining time (this is the
standard "clamp your accumulator" rule):

```cpp
while (leftover_ >= step_ && steps < maxStepsPerFrame_) { leftover_ -= step_; ++steps; }
if (steps == maxStepsPerFrame_ && leftover_ >= step_)
    leftover_ = 0.0f;   // give up on the backlog instead of carrying it forever
```

---

## 5. `TRIGGER_` bounds and `SPAWN_` facing ignore the level root transform (medium)

**File:** `engine/asset/LevelBuilder.cpp:151-180`

```cpp
} else if (isSpawn) {
    ...
    const glm::mat4 world = m.restNodeWorldMatrix(nodeIndex);   // 154
    const glm::vec3 forward = -glm::vec3(world[2]);
    ...
} else if (isTrigger) {
    ...
    const glm::mat4 world = m.restNodeWorldMatrix(nodeIndex);   // 164
    ... trigger.worldMin = mn; trigger.worldMax = mx;           // 175-177
```

**What is wrong:** `GltfModel::restNodeWorldMatrix()` walks the *glTF* node
chain only (`engine/asset/GltfAsset.cpp:403-412`); it knows nothing about the
`rootTransform` passed to `instantiate()` or about `snapLevelToFloor()`, which
moves the level root afterwards. Both markers therefore bake **model space**
numbers into a field that every consumer interprets as **world space**:

* `TriggerVolume::worldMin/worldMax` are compared directly against the player's
  world AABB in `game/main.cpp:333-334`, and the debug draw boxes them with
  `DebugDraw::addBox` in world space. Any level instantiated with a non-identity
  root — which is the normal case: `spawnGltfLevel` passes a position, a yaw
  (180°/165°) *and* a scale (Fox uses 0.012) — gets trigger volumes at the wrong
  place, wrong orientation and wrong size (a 0.012-scaled level produces a
  trigger box ~80× too large, sitting near the world origin).
* `SpawnPoint::forward` is direction-only, so a level root yaw rotates the model
  but leaves the spawn's facing at the unrotated model orientation.

Note this is *not* the limitation already listed in `NOTES.md` ("a trigger
attached to a moving node does not follow it"): the placement is wrong from the
very first frame, before anything moves.

**Suggested fix:** compute both from the entity's world matrix after the
hierarchy is up to date, i.e. mirror what `AABBCollider` does — store the local
box and let `Scene::refreshColliderBounds()` (or a dedicated
`refreshTriggerBounds()`) transform it, so the root transform, the animated
snap and later patrol motion all apply. At minimum, multiply the
`restNodeWorldMatrix` result by the root's accumulated transform, and derive
`SpawnPoint::forward` from `scene.worldMatrix(entity)` after
`updateWorldMatrices()` instead of from the glTF node.

---

## 6. `Texture::Params::srgb` is inert — colour maps are never sRGB-decoded (medium)

**File:** `engine/render/Texture.cpp:27-30` and `:56-59`; the contract it breaks
is documented in `engine/render/Material.hpp:20` and `engine/render/Texture.hpp:25`

```cpp
*this = Texture(GL_RGBA8, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1,
    GL_RGBA, GL_UNSIGNED_BYTE, pixels, params);
stbi_image_free(pixels);
srgb_ = params.srgb;     // recorded, never used
```

**What is wrong:** every texture is allocated with the plain `GL_RGBA8` internal
format regardless of `params.srgb`, and the CPU path does not convert either.
`srgb_` is only read back by `Texture::isSrgb()`, which nothing calls
(`grep` across `engine/` and `game/` finds no consumer). The glTF loader
faithfully passes `srgb = true` for base-colour maps
(`engine/asset/GltfAsset.cpp:512`) and `false` for normal/metallic-roughness
maps, and `Material.hpp` claims "sRGB decoding happens in the texture at load
time when srgb=true" — but no decode happens anywhere:

* `shaders/mesh.frag` multiplies the raw texel into `albedo`
  (`albedo *= texture(uBaseColorTex, vUv).rgb`), i.e. treats gamma-encoded data
  as linear;
* lighting is then computed in that (wrong) space, and
  `shaders/post.frag` applies `pow(color, 1/2.2)` at the very end.

The net effect is that textured and untextured surfaces are lit in different
colour spaces and every base-colour map comes out too bright / desaturated,
while the metallic-roughness and normal maps (correctly treated as linear) do
not. This contradicts both `NOTES.md` ("base color (sRGB)") and the header
comments.

**Suggested fix:** pick the internal format from the flag, so the hardware does
the decode for free:

```cpp
const GLenum internal = params.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
```

and have the file/memory constructors pass that (the manual-upload constructor
already takes `internalFormat`, so the change is confined to the two loaders).
`Material`'s albedo factors and the emissive colour from glTF are already
linear, so no shader change is needed.

---

## 7. `Scene::attachTo()` accepts cycles and unvalidated ids (low)

**File:** `engine/scene/Scene.cpp:75-84`

```cpp
void Scene::attachTo(EntityId child, EntityId parent)
{
    HOR_CHECK(child != parent);
    EntityId oldParent = transforms_[child].parent;
    ...
    transforms_[parent].children.push_back(child);
}
```

**What is wrong:** the only guard is "not its own parent". Reparenting an
ancestor under its own descendant, or passing an id that does not exist
(`parent >= transforms_.size()`, `kInvalidEntity` is fine but `0xFFFFFFFE` is
not), writes out of bounds or builds a cycle. A cycle is not detected later: in
`updateWorldMatrices()` a cyclic node has a non-`kInvalidEntity` parent, so it
is never visited as a root, and the depth guard at
`engine/scene/Scene.cpp:95-98` is never reached — the whole subtree silently
keeps stale world matrices instead of a diagnosable failure. (`HOR_CHECK` being
a no-op in release — finding 2 — makes the self-parent guard vanish too.)

**Suggested fix:** validate the parent id (`parent < transforms_.size()`), walk
`parent`'s ancestor chain before attaching and reject/ignore the attach if
`child` is found (log an error and return), and drop the reliance on
`HOR_CHECK` for this.

---

## 8. Post pass hardcodes the camera's near/far planes (low)

**File:** `engine/render/Renderer.cpp:473-474`

```cpp
postShader_->setUniform("uNear", 0.05f);
postShader_->setUniform("uFar", 200.0f);
```

**What is wrong:** `shaders/post.frag` reconstructs linear depth from the depth
buffer with those two constants to compute exponential-squared fog. The real
planes live on `Camera` (`engine/render/Camera.hpp:23-24`, defaults 0.05/200)
and are public and mutable; `Camera::update()` feeds them to
`glm::perspective`. Any game or tool that changes `nearPlane`/`farPlane` (a
common thing to do for a flashlight-driven horror scene with a large far plane)
gets fog densities that no longer correspond to distance, and there is no
compile-time link between the three places. The same magic numbers also exist in
`shaders/post.frag`.

**Suggested fix:** pass the camera into `postPass()` (or cache the planes in the
renderer each frame from `camera.nearPlane/farPlane`) and upload those instead
of literals.

---

## 9. `Window::resizedSinceLastFrame` can never be `true` for a caller (low)

**File:** `engine/core/Window.cpp:126-130` (flag set at `:95`, declaration and
doc comment at `engine/core/Window.hpp:62-64`)

```cpp
void Window::pollEvents() const
{
    glfwPollEvents();
    // Cleared after every poll: the flag describes "the last polled batch".
    const_cast<Window *>(this)->resizedSinceLastFrame = false;
}
```

**What is wrong:** the framebuffer-size callback sets the flag to `true` during
`glfwPollEvents()`, and `pollEvents()` immediately clears it before returning.
Nothing in the engine reads the flag (`grep` finds only the three writes), so
today it is only dead code — but it is a public, documented API ("True on frames
where a resize happened; lets the game adjust camera aspect") that is guaranteed
to read `false` no matter when a client checks it. Any gameplay code that
starts using it will silently never see a resize.

**Suggested fix:** clear the flag at the *start* of the frame (before
`glfwPollEvents()`), e.g. in `pollEvents()` set it to `false` first and let the
callback set it back to `true`, or add a `consumeResized()` accessor that
returns and clears.

---

## 10. `decodeBase64()` overflows a signed `int` (low, undefined behaviour)

**File:** `engine/asset/GltfAsset.cpp:299-311`

```cpp
int bits = 0, accumulated = 0;
for (unsigned char c : in) {
    ...
    accumulated = (accumulated << 6) | v;   // 306
    bits += 6;
    if (bits >= 8) { bits -= 8; out.push_back((accumulated >> bits) & 0xFF); }
}
```

**What is wrong:** `accumulated` is never masked after a byte is extracted, so it
is shifted left by six bits on every input character. After six characters it
exceeds 31 bits and signed left shift / signed overflow is undefined behaviour
(compilers do in practice wrap, and the decoded bytes stay correct because only
the low `bits+8` bits are read, but this is UB that UBSan will flag, and it is
reachable from any glTF that embeds an image as a `data:` URI —
`engine/asset/GltfAsset.cpp:479-483`). A `data:` URI of a 1 MP texture is ~1.3 M
iterations of UB.

**Suggested fix:** mask to the bits that are still needed, or use an unsigned
accumulator:

```cpp
accumulated = ((accumulated << 6) | v) & 0xFFFFFF;   // keep 24 bits
```

(or `uint32_t accumulated`, which makes the shift well-defined).

---

## 11. Looping clips drop animation events that cross the loop point (medium)

**File:** `engine/anim/AnimationSystem.cpp:293` (guard), with the wrap handling
it disables at `engine/anim/AnimationSystem.cpp:181-198`

```cpp
// update(), after wrapping newTime into [0, duration)
if (!anim.currentLooping || newTime >= oldTime)
    fireEvents(*current, oldTime, newTime, anim.currentLooping, anim.onEvent);
```

**What is wrong:** when a looping clip wraps, `newTime` is strictly smaller
than `oldTime`, so this guard skips the `fireEvents()` call entirely — on
exactly the frame that crossed the loop point. `fireEvents()` has a dedicated
branch for that case (`if (looping && to < from)`, lines 187-192, which fires
`(from, duration]` plus `[0, to]`), and the guard makes it unreachable: the
only call site in the codebase is this one. So every `AnimationEvent` in the
wrapped interval is silently dropped once per loop.

Consequences for anything that uses events (footsteps, hit windows, sound
cues): an event at the very end of a clip never fires at all (its only
occurrence is in the wrapped interval); an event near the start fires on the
first playthrough and then never again, because the wrap frame swallows it.
There is no error, no log — the callback simply is not called.

**Suggested fix:** call it unconditionally and let `fireEvents` decide, since
it already distinguishes wrapping from monotonic advance:

```cpp
fireEvents(*current, oldTime, newTime, anim.currentLooping, anim.onEvent);
```

(`evaluate()` does not fire events, so nothing else needs to change; a
non-looping clip has `from <= to` by construction, and the `(from, to]` test
still gives the same result.)

---

## 12. `Scene::view()` invalidates its own iterator when a callback mutates the store (medium)

**File:** `engine/scene/Scene.hpp:69-76`; the erase it collides with is
`engine/scene/Scene.cpp:60-66`, insertion is `engine/scene/Scene.hpp:50-54`

```cpp
template<typename T, typename Fn>
void view(Fn &&fn)
{
    auto &store = storesFor<T>();
    for (auto &[id, comp] : store)   // range-for over the unordered_map
        if (valid(id))
            fn(id, comp);
}
```

**What is wrong:** `view()` hands the callback a mutable reference *and* keeps
iterating the container the reference came from. The documented component
pattern makes both dangerous mutations natural inside a callback:

* `scene.destroy(e)` erases `e` from that same `unordered_map` (`Scene::destroy`
  drops every store entry, `Scene.cpp:60-66`). Erasing the element the loop is
  currently on invalidates the loop's iterator; the implicit `++it` then runs on
  a dead iterator — undefined behaviour, in practice a skipped element or a
  crash in the bucket walk.
* `scene.add<T>(...)` / `scene.create(...)` inserts into the map. A rehash
  invalidates *every* iterator, so the iteration walks freed bucket storage.

This is not hypothetical style-policing: `updateTriggers()`
(`game/main.cpp:311-336`) is a `view<TriggerVolume>` whose whole purpose is to
fire on entry, and `TriggerVolume` already carries `once`/`fired` fields for the
one-shot case. The first gameplay code that despawns a fired trigger — or adds
a pickup when a trigger fires — hits this. Right now nothing erases inside a
`view`, so the defect is latent.

**Suggested fix:** snapshot the ids first, then look each one up (also makes the
"component removed by an earlier callback" case safe):

```cpp
template<typename T, typename Fn>
void view(Fn &&fn)
{
    auto &store = storesFor<T>();
    std::vector<EntityId> ids;
    ids.reserve(store.size());
    for (auto &[id, comp] : store)
        if (valid(id)) ids.push_back(id);
    for (EntityId id : ids)
        if (auto *comp = find<T>(id)) fn(id, *comp);
}
```

Alternatively document "do not add/remove components inside a view" and add a
deferred-destroy queue; the snapshot is cheaper than debugging the crash.

---

## 13. glTF index values are never validated against the vertex count (medium)

**File:** `engine/asset/GltfAsset.cpp:620-631`, consumed at `:636-639` by
`generateNormals()` (`:317-334`) and `generateTangents()` (`:336-...`)

```cpp
if (prim.indices) {
    geometry.indices.resize(prim.indices->count);
    for (cgltf_size k = 0; k < prim.indices->count; ++k) {
        cgltf_uint index = 0;
        cgltf_accessor_read_uint(prim.indices, k, &index, 1);
        geometry.indices[k] = index;          // no bounds check against pos->count
    }
}
```

**What is wrong:** `cgltf_accessor_read_uint()` converts a number out of the
buffer; it does not know or care that the value must name a vertex. Nothing in
this loader compares indices against `pos->count`, and the very next step uses
them as subscripts:

```cpp
Vertex &a = verts[indices[i]];   // GltfAsset.cpp:322 (same in generateTangents)
a.normal += n;                   // writes through the out-of-range reference
```

So a glTF whose index accessor references a vertex past POSITION — a truncated
re-export, a hand-edited asset, a 16-bit index from a tool that merged meshes
wrong — reads and *writes* out of bounds during load. This is heap corruption,
not a visual glitch, and it happens before any validation the engine does
(`Mesh`'s `HOR_CHECK`s do not look at indices at all, and are compiled out in
the default build anyway — finding 2). If the accessor *does* supply normals so
the generators are skipped, the bad index instead reaches `glDrawElements`,
which reads past the end of the index buffer.

**Suggested fix:** range-check while copying; one comparison per index is free
relative to the file I/O:

```cpp
cgltf_uint index = 0;
cgltf_accessor_read_uint(prim.indices, k, &index, 1);
if (index >= pos->count) {
    HOR_LOG_WARN("glTF mesh '{}' primitive {}: index {} >= {} vertices; skipping primitive",
        mesh.name, pi, index, pos->count);
    bad = true;
    break;
}
geometry.indices[k] = index;
```

and `continue` the primitive loop when `bad` is set (also drop the primitive if
`prim.indices->count % 3 != 0`, which the triangle generators currently just
truncate silently).

---

## 14. Standing up from a crouch ignores headroom (low)

**File:** `engine/scene/CharacterController.cpp:119` (`targetHeight`) and
`:319-321` (the lerp), with the overlap helper at `:135-146`

```cpp
float targetHeight = wantCrouch ? cc->crouchHeight : cc->height;   // 119
...
cc->currentHeight = glm::mix(cc->currentHeight, targetHeight, std::min(dt * 10.0f, 1.0f));
```

**What is wrong:** releasing Crouch immediately sets the target back to the full
standing height, and `currentHeight` (which *is* the collider's height, see
`playerHalf()`) expands with no `overlapsAny()` test. A player who crouches
under a 1.2 m ledge, vent or stairwell and stands up grows the box into the
ceiling collider from the inside. The resolution cannot recover from that:
`collideAxis()` deliberately ignores colliders the body was already inside
before the move (that is what keeps the player from being ejected out of the
floor, `:196-200`), so nothing pushes the body back down or blocks the growth.
The visible result is the camera/head clipping through solid geometry, plus the
body being able to occupy gaps shorter than the standing height — exactly the
spaces the level author used a low ceiling to forbid.

**Suggested fix:** only grow when there is room, and re-check every step rather
than latching the target:

```cpp
if (targetHeight > cc->currentHeight) {
    const float saved = cc->currentHeight;
    cc->currentHeight = targetHeight;          // probe at the target size
    if (overlapsAny()) {
        cc->currentHeight = saved;             // no headroom: stay low
        targetHeight = saved;
    }
}
cc->currentHeight = glm::mix(cc->currentHeight, targetHeight, std::min(dt * 10.0f, 1.0f));
```

(`overlapsAny()` already ignores `resolveCollisions == false` geometry, so a
trigger volume will not block standing up.)

---

## 15. A mesh with no material casts a shadow but is never drawn (low)

**File:** `engine/render/Renderer.cpp:380` (main pass) versus `:322-323`
(shadow pass); the item is queued at `:197-203` with no material check

```cpp
// shadowPass
if (!item.castsShadow || !item.mesh)   // 322
    return;                             // material never consulted

// mainPass
if (!item.material || (item.bones != nullptr) != skinnedPass)   // 380
    continue;
```

**What is wrong:** `buildQueue()` enqueues every visible `MeshRenderer` with a
non-null mesh regardless of `material`, and `LevelBuilder` can legitimately
leave it null (`LevelBuilder.cpp:205-209` falls back to `nullptr`
when a primitive's material index is out of range). The shadow pass draws such
an item (its shaders do not need a material), while the main pass silently
skips it — no log, no `itemsCulled` increment. The object is therefore
invisible yet casts a fully lit-looking shadow, and the stats report a shadow
draw for something that was never on screen. The comment above the main-pass
loop even claims the opposite ("a material without one still draws", meaning a
material without a *shader*).

**Suggested fix:** pick one behaviour and make both passes agree. Cheapest is
to draw with the defaults the renderer already has (placeholder texture, default
uniform values) by removing the `!item.material` early-out and guarding each
field read; otherwise skip the item in `buildQueue()` with
`HOR_LOG_WARN_ONCE("mesh '{}' has no material", ...)` so the missing material
is visible instead of magic.

---

## 16. `tuneFlashlight()` has no effect: `render()` overwrites it every frame (low)

**File:** `engine/render/Renderer.cpp:117-125` (the setter) and `:265-270`
(the reset at the top of `render()`), overwritten again at `:277-282`

```cpp
void ForwardRenderer::tuneFlashlight(float innerDeg, float outerDeg, float range, float intensity)
{
    flashlightInnerDeg_ = innerDeg;      // 121-124
    ...
}

void ForwardRenderer::render(Scene &scene, Camera &camera)
{
    flashlightInnerDeg_ = 18.0f;         // 265: back to the hardcoded defaults
    flashlightOuterDeg_ = 24.0f;
    flashlightRange_ = 45.0f;
    flashlightIntensity_ = 8.0f;
    ...
    scene.view<SpotLight>([&](EntityId id, const SpotLight &sl) {
        flashlightInnerDeg_ = sl.innerConeDeg;   // 277: and then the component wins
        ...
    });
```

**What is wrong:** the four tuned members are reset to literals at the start of
every frame and then overwritten from the first `SpotLight` component, so any
value written through `tuneFlashlight()` is discarded before the next shadow /
lighting pass reads it. The API is public, documented as the gameplay hook for
"battery flicker / cone widening while sprinting", and is a silent no-op — the
next person to wire sprint-widening will see no change and go looking in the
shader. There are no callers today (`grep` across `engine/` and `game/` finds
only the definition), so this is latent, not currently visible.

**Suggested fix:** apply the tuning *after* the component read (e.g. as
multipliers/offsets applied to the gathered values), or move the gathered
state into locals and let `tuneFlashlight()` own the defaults. If neither is
wanted yet, deleting the setter is better than shipping a hook that cannot
work.

---

## 17. Texture cache ignores the per-use sRGB flag (low)

**File:** `engine/asset/GltfAsset.cpp:460` (cache), `:462-466` (lookup),
`:494` (insert); the differing flags are passed at `:514`, `:517` and `:523`

```cpp
std::unordered_map<const cgltf_texture *, std::shared_ptr<Texture>> textureCache;
auto loadTexture = [&](const cgltf_texture *tex, bool srgb) -> std::shared_ptr<Texture> {
    auto it = textureCache.find(tex);        // key is the texture only
    if (it != textureCache.end())
        return it->second;                   // returns the first use's decode flag
    ...
    params.srgb = srgb;
    textureCache[tex] = texture;              // only the first caller's flag is honoured
};
```

**What is wrong:** the key is the `cgltf_texture*` alone, but the same texture
is requested with different `srgb` values depending on the slot: base colour
wants `true`, normal and metallic-roughness want `false`. Whichever material is
loaded first decides the decode for every later user of that image. Reusing one
texture as both an albedo map and a data map is legal glTF, and the engine
needs both interpretations.

Today this is masked by finding 6 (`srgb` is recorded but never used), which is
why it is listed as low — but it is an independent defect: fixing finding 6 by
selecting `GL_SRGB8_ALPHA8` without changing this key will silently mis-decode
one of the two uses, and the symptom (a normal map read as sRGB, or an albedo
map read as linear) will look like a shader bug.

**Suggested fix:** include the flag in the key, or key on the image instead
(two `cgltf_texture` objects may share one `cgltf_image`):

```cpp
std::map<std::pair<const cgltf_image *, bool>, std::shared_ptr<Texture>> textureCache;
```

---

## 18. `Input::released()` bypasses the ImGui keyboard gate (low)

**File:** `engine/core/Input.cpp:259-268`, versus `:271-287`

```cpp
bool Input::released(Action a) const
{
    const auto &map = bindings_[(size_t)a];
    for (int key : map) {
        if (key == kUnknown)
            continue;
        if (keys_[key].wasDown && !keys_[key].isDown)   // raw state only
            return true;
    }
    return false;
}

bool Input::keyHeld(int key) const
{
    ...
    if (!mouseCaptured_ && keyboardWantedByImGui)
        return false;   // overlay eats keyboard while visible + focused
```

**What is wrong:** `keyHeld()` and `keyPressed()` both suppress input while an
ImGui text field owns the keyboard; `released()` does not. With the F1 overlay
open and a field focused, pressing a bound key reports nothing, but releasing it
reports a release — an edge with no matching press, from input the engine
explicitly declared as UI input. Nothing consumes `released()` for gameplay
today (only the key-edge self-test at `game/main.cpp:801`), so this is latent,
but any future "release to stop firing / release to stop sprinting" action gets
a spurious trigger the moment the player types in the overlay.

**Suggested fix:** give `released()` the same early-out, or factor the gate into
a helper used by all three:

```cpp
bool Input::actionKeyActive(int key) const
{
    return key >= 0 && key <= GLFW_KEY_LAST
        && !(!mouseCaptured_ && keyboardWantedByImGui);
}
```

---

## 19. Head-bob is folded into the eye twice (medium)

**File:** `engine/scene/CharacterController.cpp:359-368` and
`engine/render/Camera.cpp:47`; the intended split is documented at
`engine/render/Camera.hpp:12-14` ("controller writes a bobOffset + bobRoll ...
camera folds it into view/orientation") and `:29`

```cpp
// CharacterController.cpp:366-367 -- the eye already includes the bob, and the
// same offset is then published for the camera to fold in again.
hooks.camera->position = xf.position + glm::vec3(0.0f, eyeHeight, 0.0f) + cc->bobOffset;
hooks.camera->bobOffset = cc->bobOffset;

// Camera.cpp:47 -- added a second time when the view matrix is built.
glm::vec3 eye = position + bobOffset;
```

**What is wrong:** the controller folds the head-bob into `Camera::position`
*and* stores the same offset in `Camera::bobOffset`; `Camera::update` then adds
it once more. The rendered eye is `feet + eyeHeight + 2 * bob`, so the walk bob
is 6 cm instead of 3 cm (`kBobVertical`) and the sway 8 cm instead of 4 cm
(`kBobLateral`), while the roll stays at its authored amplitude because it is
only applied inside `Camera::update` (`:55-57`). The offset and the roll no
longer describe the same head motion.

The double offset also undoes the measured improvement from NOTES bug 23. The
landing-bob self-test (`game/main.cpp:830-850`) measures
`cam.position.y - feet.y`, i.e. *one* of the two offsets, so the 2.0 mm landing
step it reports is about 4 mm of real eye motion and the "5.6 mm worst" over the
jump is about 11 mm — past the test's own 10 mm threshold. In other words the
regression test cannot see this bug: it measures the controller, not the view.
A smaller inconsistency: `uCameraPos` (specular) is uploaded from
`camera.position`, which carries one bob, while the view matrix is built from
the eye carrying two, so highlights and geometry disagree about where the eye
is while walking.

**Suggested fix:** give the offset a single owner. Keep the controller
authoritative for `position = feet + eyeHeight` and let `Camera::update` add
`bobOffset` (then rotate it as in finding 20), or drop the addition in
`Camera::update` and delete the duplicate field. Whichever way, make
`Camera::position` the value the view actually uses so the flashlight and the
self-test agree with the image.

---

## 20. Head-bob sway is applied along world +X, not camera-right (low)

**File:** `engine/scene/CharacterController.cpp:346-349`, consumed at
`engine/render/Camera.cpp:47`; `Camera.hpp:29` calls the field a "world-space eye
offset along local up/right"

```cpp
cc->bobOffset.y = -std::abs(std::sin(cc->bobPhase)) * kBobVertical * amp;
cc->bobOffset.x = std::cos(cc->bobPhase * 0.5f) * kBobLateral * amp;  // "sideways"
cc->bobOffset.z = 0.0f;
...
glm::vec3 eye = position + bobOffset;   // added straight to the world position
```

**What is wrong:** `bobOffset.x` is the side-to-side sway, but it is added to
the world-space eye along +X, which equals camera-right only at yaw 0.
Camera-right is `(cos yaw, 0, -sin yaw)`: at yaw 90 degrees the sway is exactly
along the view axis (a forward/back pump), and at yaw 180 degrees it is
inverted left/right. Walking while facing east or west makes the camera breathe
toward and away from the wall instead of swaying side to side. The amplitude is
small (4 cm, doubled to 8 cm by finding 19), but this is the one component of
the bob that is supposed to be lateral, and it silently changes character with
the yaw the player happens to be facing. `bobOffset.z` is always 0, so nothing
rotates the offset into the camera basis.

**Suggested fix:** treat the offset as camera-local, e.g.
`eye += right() * bobOffset.x + up() * bobOffset.y` inside `Camera::update`
(after the roll, if the sway should follow the head), or rotate it by yaw in
the controller. Do it in one place, together with finding 19.

---

## 21. The flashlight is whichever `SpotLight` the store visits last (low)

**File:** `engine/render/Renderer.cpp:265-286`; spot lights are imported as
components by `engine/asset/LevelBuilder.cpp:117-124`

```cpp
flashlightInnerDeg_ = 18.0f;                  // defaults at the top of render()
...
scene.view<SpotLight>([&](EntityId id, const SpotLight &sl) {
    flashlightInnerDeg_ = sl.innerConeDeg;    // overwritten for every light
    ...
    flashlightPos_ = glm::vec3(world[3]);
    flashlightDir_ = dir / len;               // last visited wins
    hasFlashlightTransform_ = true;
});
```

**What is wrong:** the renderer has one flashlight slot, but the scene can hold
several `SpotLight`s: the game's camera light plus any `KHR_lights_punctual`
spot imported from a level. `scene.view` walks an `unordered_map` keyed by
entity id, so the winner depends on bucket layout, not on authoring or creation
order. If a level spot light is visited last, the shadow map, the cone and the
beam origin follow that fixed fixture and the player's flashlight stops
tracking the camera; the debug draw (`:500`) still draws both, so the scene
looks like it has two correctly placed lights. Adding unrelated entities can
rehash the map and change the winner between runs.

**Suggested fix:** address the flashlight explicitly — e.g.
`setFlashlight(EntityId)` called by gameplay and a direct component lookup —
and skip other spot lights in the gather (or support N spot lights properly
later). When no flashlight is set, keep the existing camera-basis fallback.

---

## 22. A failed shader hot reload is recorded as success and never retried (low)

**File:** `engine/render/Shader.cpp:226-241`

```cpp
bool ok = tryLoadProgram();      // false: compile/link failed, old program kept
vertRef_.lastWrite = vt;         // ...but the new mtimes are committed anyway
fragRef_.lastWrite = ft;
if (ok) { reflectUniforms(); HOR_LOG_INFO("shader reloaded [{}]", name_); }
```

**What is wrong:** the comment above (`:228-231`) promises that a mid-save
truncation is skipped and retried next frame, but that only happens when the
*vertex* file cannot be opened (`probe`, `:233`). The normal mid-save state is
both files readable and the new source failing to compile; `tryLoadProgram()`
then returns false, and because `lastWrite` is still advanced,
`checkHotReload()` sees no change on later frames. The old program keeps
running (which is correct), but the reload is lost until the file changes
again. An editor that truncates and rewrites within one timestamp tick, or a
tool that copies with the original mtime, leaves the old shader running
indefinitely even though the file on disk is the fixed version. The failure is
only visible as one error line from `tryLoadProgram`, and `lastError_` is not
surfaced anywhere else.

**Suggested fix:** only commit `lastWrite` when `ok` is true; on failure keep
the old stamps so the next frame retries (rate-limit the error log so a
permanently broken shader cannot spam). Probe both stages for readability
before rebuilding, not just the vertex file.

---

## 23. The sub-step clamp can exceed its own movement bound on long falls (low)

**File:** `engine/scene/CharacterController.cpp:30-31` (constants) and
`:266-273`; the guarantee is stated at `:24-30` and in `NOTES.md` ("Sub-stepping
caps movement at 8 cm per collision pass (8 passes max per fixed step)")

```cpp
const float travel = glm::length(cc->velocity) * dt;
const int substeps = std::clamp(
    static_cast<int>(std::ceil(travel / kMaxSubStep)), 1, kMaxSubSteps);  // 8
const float h = dt / static_cast<float>(substeps);
```

**What is wrong:** the pass count is clamped to 8 but the distance is not, so
once `travel > kMaxSubSteps * kMaxSubStep` (0.64 m) a single pass moves further
than the 8 cm the comment says is the maximum, and the "a fast fall can never
cross a collider between two overlap tests" claim no longer holds. Vertical
velocity has no terminal clamp (gravity is -13; `velocity.y` is only assigned
by the jump impulse and by landing), so the bound can be exceeded by ordinary
falling: after about 2.3 s (v ~= 30 m/s, ~34 m dropped) each pass is already
6 cm — the thin-wall thickness used by the self-test — and after 4 s
(v ~= 52 m/s) it is 11 cm. At that point `collideAxis` can start a pass in
front of a collider and end behind it without ever overlapping. The current
corridor has no such drop, so this is latent; a pit, a stairwell or any
velocity impulse would expose it. The rest of the resolver is sound; crouch
growth is finding 14.

**Suggested fix:** derive the pass count from the actual distance and do not
clamp it below what the distance needs (cap at, say, 64 and handle the
remaining move explicitly), or clamp fall speed to
`kMaxSubSteps * kMaxSubStep / dt` (~38 m/s at 60 Hz), which makes the existing
8-pass budget provably sufficient.

---

## 24. Skipped marker nodes lose their transform for their children (low)

**File:** `engine/asset/LevelBuilder.cpp:85-95`

```cpp
if ((isCollision && !options.importCollision) || (isSpawn && !options.importSpawnPoints)
    || (isTrigger && !options.importTriggers)) {
    // Still walk children so a disabled marker doesn't orphan a subtree;
    // children attach to the parent's entity instead.
    for (int child : node.children)
        buildNode(child, parentEntity);
    return;
}
```

**What is wrong:** when an import option turns a marker node off, no entity is
created for it, so its local translation/rotation/scale is never applied; its
children are built directly under `parentEntity` with their own local TRS,
which is relative to the skipped node. Every descendant is therefore placed as
if the marker sat at the parent's origin. With `importTriggers = false` (a
render-only or collision-only import), a `TRIGGER_` node with a translation —
which is how Blender authors place one — silently moves its whole subtree. The
defaults import all three kinds, so the bug is dormant until a caller uses the
options, but the comment presents the re-parenting as harmless when it is only
harmless for an identity transform.

**Suggested fix:** create a transform-only entity for the skipped node (no
components, so it costs one Transform) and use it as `parentEntity`; or
pre-multiply the skipped node's local matrix into each descendant's local
transform before recursing.

---

## 25. A `TRIGGER_` node without a mesh becomes a zero-size volume at the world origin (low)

**File:** `engine/asset/LevelBuilder.cpp:160-180`; `TriggerVolume` defaults at
`engine/scene/Components.hpp:67-70`

```cpp
TriggerVolume trigger;
trigger.tag = node.name.substr(8);
if (haveBounds) {
    ... trigger.worldMin = mn; trigger.worldMax = mx;
}
scene.add(entity, trigger);   // no bounds: worldMin == worldMax == (0,0,0)
```

**What is wrong:** `haveBounds` is false when the node has no mesh (or all its
primitives were skipped), and the component is added anyway. `TriggerVolume`'s
fields are *world-space* and default to zero, so the trigger is not at the
node: it is a degenerate box at the map origin. The runtime test in
`game/main.cpp` (`updateTriggers`) only needs the player's body box to straddle
it, so a body below y = 0 near the origin fires it, and the debug draw shows a
zero-size blue box at the origin with no indication of which node it belongs
to. `COL_` nodes in the same situation warn and skip; `TRIGGER_` is silent, so
the author gets neither a volume in the right place nor a message saying why.

**Suggested fix:** warn and skip the component when there are no bounds
(matching `COL_`), or build a default volume from extras / the node transform
so the trigger is still usable.

---

## Checked and found clean

Worth recording so the review is not read as exhaustive-by-omission: the
transform/bone-space math in `AnimationSystem::evaluate` (mesh-node inverse,
joint positions, crossfade weights — the event bookkeeping beside it is finding
11), `Mesh` primitive winding/normal/tangent generation, the axis-separated
swept collision resolver (step-up and ground probes; the sub-step budget is
finding 23 and crouch growth finding 14), the GL object lifetimes
(`Scene`/`AssetLibrary`/`Renderer`/`Window` member order all destroy GL objects
before the context), `Shader` hot-reload failure handling (a failed compile keeps
the previous program and does not leak the stages; losing the retry is finding
22), `Framebuffer` depth-only setup, and the flight-recorder ring buffers
(previous-slot indexing and the rate-limited dump are correct).

The `--selftest` coverage was checked too, with one caveat: the landing-bob
check reads `Camera::position`, i.e. only half of the rendered eye motion, so it
cannot see finding 19; the other movement, trigger and mouse checks do exercise
the paths they claim to.

On the second pass the shader pair was checked too: `mesh.vert` and
`shadow_depth.vert` build the same LBS matrix from the same UBO layout (so
skinned shadows track the body), `sampleShadow` remaps NDC correctly and rejects
out-of-frustum samples, `post.frag`'s linear-depth reconstruction matches the
perspective matrix `Camera::update` builds (the constants are wrong, finding 8,
not the formula), and the fullscreen-triangle `post.vert` covers the viewport
with matching UVs. Also verified clean: the `Input` re-baseline windows (sample
and frame counters, warp drop, focus sampling), and the identity/order of
`Scene::destroy`'s store erases for a leaf entity. The engine is single-threaded
by design (`Logger`, `Input::active_`, `FlightRecorder` are all touched only from
the main thread), so no data races were found.
