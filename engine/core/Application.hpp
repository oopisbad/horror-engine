#pragma once

#include "asset/GltfAsset.hpp"
#include "core/Input.hpp"
#include "core/Time.hpp"
#include "core/Window.hpp"
#include "render/Camera.hpp"
#include "render/Renderer.hpp"
#include "scene/Scene.hpp"

#include <functional>
#include <string>

namespace horror {

class ImGuiOverlay;

// Central wiring: owns Window/Input/Scene/Camera/Renderer, runs the main
// loop, and mediates game-supplied update hooks. Gameplay stays out of this
// file; everything game-specific comes in as callbacks + the Scene contents.
class Application {
public:
    struct Config {
        std::string windowTitle = "horror";
        uint32_t windowWidth = 1280;
        uint32_t windowHeight = 720;
        bool fullscreen = false;
        bool visible = true;
        bool vsync = true;
        unsigned frameRateLimit = 0;
        bool wireframe = false; // start with polygon mode LINE (--wireframe)
        float fixedStepSeconds = 1.0f / 60.0f;
        uint32_t shadowMapSize = 2048;
    };

    explicit Application(Config config);
    ~Application();
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    // Blocks until quit; returns process exit code.
    int run();
    void quit() { running_ = false; }

    // --- systems access (game owns content, engine owns loop) --------------
    Window &window() { return window_; }
    Input &input() { return input_; }
    Camera &camera() { return camera_; }
    Scene &scene() { return scene_; }
    ForwardRenderer &renderer() { return renderer_; }
    AssetLibrary &assets() { return assets_; }

    // Gameplay hooks ---------------------------------------------------------
    // Fixed timestep: physics/player logic. dt is exactly Config::fixedStep.
    std::function<void(float)> onFixedStep;
    std::function<void()> onDrawUI;
    std::function<void()> onEscape;
    bool simulationEnabled = true;
    bool worldVisible = true;
    bool gameplayInputEnabled = true;
    // Variable per-frame pre-render: animations, camera smoothing, etc.
    std::function<void(float)> onFrameUpdate;

    // Debug/capture knobs (used by game main args + the overlay).
    void requestScreenshot(const std::string &tgaPath) { screenshotPath_ = tgaPath; }
    void setFrameLimit(int frames) { frameLimit_ = frames; }
    int framesRun() const { return framesRun_; }
    // Starts hidden: first-person games want immediate capture; F1 opens.
    bool showImGui = false;

private:
    void handleHotkeys();
    void drawImGui();
    void takeScreenshot(const std::string &path);

    Config config_;
    Window window_;
    Input input_;
    Camera camera_;
    Scene scene_;
    AssetLibrary assets_; // before renderer: GL assets die before the context
    ForwardRenderer renderer_;
    std::unique_ptr<ImGuiOverlay> imgui_;

    FrameTimer frameTimer_;
    FixedStepAccumulator logic_;
    bool running_ = true;
    int exitCode_ = 0;
    int framesRun_ = 0;
    int frameLimit_ = 0; // 0 = unlimited
    float elapsed_ = 0.0f; // monotonic render clock (grain animation)
    std::string screenshotPath_;
};

} // namespace horror
