# Horror Engine — bug review

Scope: every hand-written source file under `engine/` and `game/` plus the GLSL
under `shaders/` (third-party code in `external/` was skipped). Findings were
confirmed by reading the code; nothing here is a guess or a style preference.
Items already listed under "Known issues / TODO" in `NOTES.md` (upside-down
TGA screenshots, one skin shared by several nodes, alpha modes, etc.) are
deliberately not repeated.

Severity: **high** = memory unsafety / crashes, **medium** = wrong behaviour
that will show up in normal play, **low** = latent or diagnostic defects.

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

## Checked and found clean

Worth recording so the review is not read as exhaustive-by-omission: the
transform/bone-space math in `AnimationSystem::evaluate` (mesh-node inverse,
joint positions, crossfade weights), `Mesh` primitive winding/normal/tangent
generation, the axis-separated swept collision resolver (step-up, sub-stepping,
ground probes) and its `--selftest` coverage, the GL object lifetimes
(`Scene`/`AssetLibrary`/`Renderer`/`Window` member order all destroy GL objects
before the context), `Shader` hot-reload failure handling (a failed compile keeps
the previous program and does not leak the stages), `Framebuffer` depth-only
setup, and the flight-recorder ring buffers (previous-slot indexing and the
rate-limited dump are correct). The engine is single-threaded by design
(`Logger`, `Input::active_`, `FlightRecorder` are all touched only from the main
thread), so no data races were found.
