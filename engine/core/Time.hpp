#pragma once

#include <chrono>

namespace horror {

// Wall-clock frame timing + the fixed-timestep accumulator for the main loop.
//
// Standard "fix your timestep" pattern: render every frame at whatever rate
// vsync/uncapped gives, run gameplay logic in quantized fixed steps so
// physics/collision stay deterministic across refresh rates.
class FrameTimer {
public:
    // Call once per frame; returns seconds since the previous call, clamped
    // so a debugger breakpoint or OS hiccup can't spiral the accumulator.
    float tickRealtime(float maxDelta = 0.25f)
    {
        auto now = std::chrono::steady_clock::now();
        if (started_)
            delta_ = std::chrono::duration<float>(now - last_).count();
        last_ = now;
        started_ = true;
        if (delta_ > maxDelta)
            delta_ = maxDelta;
        return delta_;
    }

    float deltaTime() const { return delta_; }
    double totalTime() const { return std::chrono::duration<double>(last_.time_since_epoch()).count(); }

private:
    std::chrono::steady_clock::time_point last_{};
    float delta_ = 0.0f;
    bool started_ = false;
};

// Consumes variable frame time and tells the game how many fixed steps to run.
class FixedStepAccumulator {
public:
    explicit FixedStepAccumulator(float stepSeconds)
        : step_(stepSeconds) {}

    // Feed last frame's delta; returns the number of fixed logic steps to run
    // this frame. Carries leftover time forward; the clamp in FrameTimer keeps
    // the value bounded.
    int stepsToRun(float frameDelta)
    {
        leftover_ += frameDelta;
        int steps = 0;
        while (leftover_ >= step_ && steps < maxStepsPerFrame_) {
            leftover_ -= step_;
            ++steps;
        }
        return steps;
    }

    float fixedStepSeconds() const { return step_; }

    // Fraction [0,1) of the way to the next fixed step, for future render
    // interpolation. Not used yet; physics is slow enough to render directly.
    float alphaFraction() const { return leftover_ / step_; }

private:
    float step_;
    float leftover_ = 0.0f;
    // Cap avoids the "death spiral" on machines that can't keep up
    // (each simulated step eating more than the time it represents).
    static constexpr int maxStepsPerFrame_ = 8;
};

} // namespace horror
