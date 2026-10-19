#pragma once

#include <string>

namespace horror {

class Application;

// Dear ImGui overlay: stats (fps / frametime / draw stats), camera state,
// and toggles (wireframe, debug draw, vsync, mouse capture). Renders after
// the world pass into the default framebuffer.
class ImGuiOverlay {
public:
    void init(Application &app);
    void shutdown();

    // NewFrame + build all widget queues (no GL touched yet).
    void frame(float deltaTime, Application &app);
    // Render the queued widgets (call before swap).
    void renderPass();

    bool wantsMouse() const;
    bool wantsTextInput() const;
    void setMouseCursorVisible(bool visible);

    void drawDebugWindows(Application &app);

private:
    bool initialized_ = false;
    std::string lastShaderReloadNote_;
};

} // namespace horror
