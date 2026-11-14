#include "debug/ImGuiOverlay.hpp"

#include "core/Application.hpp"
#include "core/Log.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <glm/glm.hpp>

#include <cstdio>

namespace horror {

void ImGuiOverlay::init(Application &app)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Ini file off: no settings cache surprises while iterating.
    io.IniFilename = nullptr;

    // GLFW chain-ordering note: engine Input callbacks were installed before
    // this init, ImGui_ImplGlfw preserves the previous handler and forwards.
    ImGui_ImplGlfw_InitForOpenGL(app.window().handle(), true);
    ImGui_ImplOpenGL3_Init("#version 450 core");
    initialized_ = true;
    HOR_LOG_DEBUG("ImGui overlay initialized");
}

void ImGuiOverlay::shutdown()
{
    if (!initialized_)
        return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void ImGuiOverlay::frame(float deltaTime, Application &)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.DeltaTime = deltaTime > 0.0f ? deltaTime : 1.0f / 60.0f;
    ImGui::NewFrame();
}

bool ImGuiOverlay::wantsMouse() const
{
    return ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() || ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiOverlay::wantsTextInput() const
{
    return ImGui::GetIO().WantTextInput;
}

void ImGuiOverlay::setMouseCursorVisible(bool visible)
{
    // Engine toggles OS capture; here we only pick the ImGui cursor shape.
    ImGuiIO &io = ImGui::GetIO();
    io.MouseDrawCursor = visible && !ImGui::IsAnyItemHovered();
}

// helper label
namespace {
void statLine(const char *label, const std::string &value)
{
    ImGui::Text("%s: %s", label, value.c_str());
}
std::string vec3String(const glm::vec3 &v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
    return buf;
}
} // namespace

void ImGuiOverlay::drawDebugWindows(Application &app)
{
    const ImGuiIO &io = ImGui::GetIO();
    if (ImGui::Begin("Engine Debug")) {
        ImGui::Text("%.1f fps (%.2f ms/frame)", io.Framerate,
            io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);

        const auto &rs = app.renderer().stats();
        ImGui::Separator();
        ImGui::Text("draw calls:      %u (culled %u)", rs.itemsDrawn, rs.itemsCulled);
        ImGui::Text("triangles:       %u", rs.triangles);
        ImGui::Text("shadow draws:    %u", rs.shadowDraws);

        const auto &cam = app.camera();
        ImGui::Separator();
        statLine("camera pos", vec3String(cam.position));
        ImGui::Text("yaw/pitch:       %.1f / %.1f degrees",
            glm::degrees(cam.yaw), glm::degrees(cam.pitch));

        // Mouse-look feel lives on the controller component, so the overlay
        // edits it in place instead of inventing a parallel settings struct.
        ImGui::Separator();
        ImGui::Text("mouse look");
        app.scene().view<CharacterController>([&](EntityId id, CharacterController &cc) {
            ImGui::PushID(static_cast<int>(id));
            // rad/pixel, shown as degrees per pixel so the number is readable.
            float degPerPixel = glm::degrees(cc.mouseLookSensitivity);
            if (ImGui::SliderFloat("sensitivity", &degPerPixel, 0.02f, 0.35f, "%.3f deg/px"))
                cc.mouseLookSensitivity = glm::radians(degPerPixel);
            ImGui::Checkbox("invert Y", &cc.invertY);
            ImGui::PopID();
        });

        ImGui::Separator();
        auto &renderer = app.renderer();
        bool wire = renderer.wireframe();
        if (ImGui::Checkbox("wireframe", &wire))
            renderer.setWireframe(wire);
        bool dd = renderer.debugDrawEnabled();
        if (ImGui::Checkbox("debug draw", &dd))
            renderer.setDebugDrawEnabled(dd);
        bool vs = app.window().vsync();
        if (ImGui::Checkbox("vsync", &vs))
            app.window().setVsync(vs);
        bool captured = app.input().mouseCaptured();
        if (ImGui::Checkbox("capture mouse", &captured))
            app.input().setMouseCaptured(captured);

        ImGui::Separator();
        ImGui::Text("post");
        auto &post = renderer.post();
        ImGui::Checkbox("post enabled", &post.enabled);
        ImGui::SliderFloat("bloom", &post.bloom, 0.0f, 0.4f, "%.2f");
        ImGui::SliderInt("volumetric steps", &post.volumeSteps, 0, 20);
        ImGui::Checkbox("Temporal anti-aliasing", &post.temporalAA);
        ImGui::Checkbox("Screen-space reflections", &post.screenReflections);
        const auto &renderStats = renderer.stats();
        ImGui::Text("Local shadow maps: %u cached, %u updated", renderStats.cachedShadowFaces, renderStats.updatedShadowFaces);
        ImGui::Text("Probes: %u   Decals: %u   Fog volumes: %u", renderStats.activeProbes, renderStats.activeDecals, renderStats.activeFogVolumes);
        ImGui::SliderFloat("exposure", &post.exposure, 0.1f, 4.0f, "%.2f");
        ImGui::SliderFloat("vignette", &post.vignette, 0.0f, 1.5f, "%.2f");
        ImGui::SliderFloat("grain", &post.grain, 0.0f, 0.1f, "%.3f");
        ImGui::SliderFloat("fog density", &post.fogDensity, 0.0f, 0.2f, "%.3f");
        ImGui::ColorEdit3("fog color", &post.fogColor.x, ImGuiColorEditFlags_Float);

        // Trigger events are gameplay state, not renderer state: show the
        // authoritative component fields so "walking onto it" is testable from
        // the overlay (the console gets an INFO line on each entry).
        ImGui::Separator();
        ImGui::Text("triggers");
        {
            int shown = 0;
            app.scene().view<TriggerVolume>([&](EntityId, const TriggerVolume &trigger) {
                ImGui::Text("%s: enters %d %s", trigger.tag.c_str(), trigger.enterCount,
                    trigger.playerInside ? "(inside)" : "");
                ++shown;
            });
            if (shown == 0)
                ImGui::Text("(none)");
        }

        ImGui::Separator();
        ImGui::Text("animation");
        bool bones = renderer.drawBones();
        if (ImGui::Checkbox("draw bones", &bones))
            renderer.setDrawBones(bones);

        // Global speed keeps every clip in sync; the combo restarts the
        // selected clip with a short crossfade.
        static float animSpeed = 1.0f;
        if (ImGui::SliderFloat("anim speed", &animSpeed, 0.0f, 3.0f, "%.2f")) {
            app.scene().view<Animator>([&](EntityId, Animator &animator) {
                animator.speed = animSpeed;
            });
        }

        int shownAnimators = 0;
        app.scene().view<Animator>([&](EntityId id, Animator &animator) {
            if (!animator.clips || animator.clips->empty())
                return;
            ++shownAnimators;
            ImGui::PushID(static_cast<int>(id));
            ImGui::Text("%s", app.scene().name(id).c_str());
            if (ImGui::BeginCombo("##clip", animator.currentClipName().c_str())) {
                for (size_t i = 0; i < animator.clips->size(); ++i) {
                    const auto &clip = (*animator.clips)[i];
                    if (!clip)
                        continue;
                    const bool selected = static_cast<int>(i) == animator.currentClip;
                    if (ImGui::Selectable(clip->name.c_str(), selected))
                        animator.playIndex(static_cast<int>(i), 0.2f, true);
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("restart"))
                animator.playIndex(animator.currentClip, 0.0f, animator.currentLooping);
            ImGui::PopID();
        });
        if (shownAnimators == 0)
            ImGui::Text("(no animated meshes loaded)");

        // Ambient is the "is the room readable?" knob; keep it very low.
        AmbientLight ambient = renderer.ambient();
        if (ImGui::ColorEdit3("ambient color", &ambient.color.x, ImGuiColorEditFlags_Float))
            renderer.setAmbient(ambient);
        if (ImGui::SliderFloat("ambient intensity", &ambient.intensity, 0.0f, 3.0f, "%.2f"))
            renderer.setAmbient(ambient);

        ImGui::Separator();
        ImGui::Text("controls: WASD move · Shift sprint · Ctrl crouch · Space jump");
        ImGui::Text("F1 UI · P wireframe · O debug draw · V vsync · Esc quit");
        ImGui::Text("F9 flight recorder dump · M patrol sample models");
    }
    ImGui::End();
}

void ImGuiOverlay::renderPass()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace horror
