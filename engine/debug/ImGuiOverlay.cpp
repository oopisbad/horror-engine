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
        ImGui::Text("controls: WASD move · Shift sprint · Ctrl crouch · Space jump");
        ImGui::Text("F1 UI · P wireframe · O debug draw · V vsync · Esc quit");
    }
    ImGui::End();
}

void ImGuiOverlay::renderPass()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace horror
