#pragma once

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace horror {

class Shader;
class Texture;

// Material = shader + surface parameters. Deliberately plain data: the
// renderer is responsible for uploading it (one place knows the uniform
// names), while gameplay only tweaks values. Keeping bind logic out of the
// material struct means later passes (deferred G-buffer, post) can reuse the
// same struct with different upload code.
//
// Color space note: albedo stays linear; sRGB decoding happens in the texture
// at load time when srgb=true.
//
// glTF import fills these from pbrMetallicRoughness; the forward shader
// samples all three texture slots (base color, tangent-space normal,
// metallic-roughness where G=roughness and B=metallic).
struct Material {
    std::string name; // debug label
    std::shared_ptr<Shader> shader;

    glm::vec3 albedo{0.7f, 0.7f, 0.7f};
    float roughness = 0.8f;
    float metallic = 0.0f;

    // Emissive contribution is added post-integration, so a fully dark room
    // with a single emissive strip still reads as a light source.
    float emissiveStrength = 0.0f;
    glm::vec3 emissiveColor{1.0f};

    float opacity = 1.0f;
    float wetness = 0.0f;
    float lightmapStrength = 1.0f;
    const Texture *lightmap = nullptr; // optional baked indirect irradiance in UV1
    float alphaCutoff = -1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    bool doubleSided = false;
    bool unlit = false;
    // Optional tiling detail with world-space coordinates for procedural geometry.
    float detailScale = 0.0f;
    Texture *detailTex = nullptr;
    // Non-owning; textures are owned by the scene or the imported model.
    Texture *baseColorTex = nullptr;         // sRGB
    Texture *emissiveTex = nullptr;
    Texture *occlusionTex = nullptr;
    Texture *normalTex = nullptr;            // linear, tangent space (OpenGL +Y)
    Texture *metallicRoughnessTex = nullptr; // linear, G=roughness B=metallic
};

} // namespace horror
