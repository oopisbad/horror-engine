#pragma once

#include <glm/glm.hpp>

namespace horror {

// Marker coordinates run from -0.5 to +0.5 on every axis.
struct FogVolume {
    glm::vec3 color{0.10f, 0.14f, 0.18f};
    float density = 0.22f;
};

struct ReflectionProbe {
    float strength = 1.0f;
};

struct SurfaceDecal {
    enum class Pattern { Grime, Leak, Scuff };
    Pattern pattern = Pattern::Grime;
    glm::vec3 tint{0.14f, 0.12f, 0.10f};
    float strength = 0.75f;
    float wetness = 0.0f;
};

} // namespace horror
