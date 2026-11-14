#pragma once

#include <glad/gl.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

namespace horror {

// GLSL program wrapper.
//
// - Hot reload: `checkHotReload()` samples file mtimes (call once per frame
//   from the app); a recompile failure keeps the previous program so the game
//   keeps running while you edit shaders.
// - Uniforms: locations are reflected once after linking and cached; setters
//   fall back to a live lookup for names we didn't anticipate.
class Shader {
public:
    Shader() = default;
    // `defines` are injected as `#define <entry>` between #version and the
    // rest of each stage, enabling static/skinned variants of one source pair.
    Shader(const std::string &vertexPath, const std::string &fragmentPath,
        const std::string &name = "", const std::vector<std::string> &defines = {});
    ~Shader();

    Shader(Shader &&) noexcept;
    Shader &operator=(Shader &&) noexcept;
    Shader(const Shader &) = delete;
    Shader &operator=(const Shader &) = delete;

    void bind() const;
    static void unbind() { glUseProgram(0); }

    bool valid() const { return program_ != 0; }
    GLuint handle() const { return program_; }
    const std::string &name() const { return name_; }

    // Returns true if a reload happened (so callers can log / refresh state).
    bool checkHotReload();

    // --- uniform setters ------------------------------------------------
    // Fast path uses the reflection cache; unknown names do a one-time lookup.
    void setUniform(const char *name, float value) const;
    void setUniform(const char *name, int value) const;
    void setUniform(const char *name, const glm::vec2 &value) const;
    void setUniform(const char *name, const glm::vec3 &value) const;
    void setUniform(const char *name, const glm::vec4 &value) const;
    void setUniform(const char *name, const glm::mat3 &value) const;
    void setUniform(const char *name, const glm::mat4 &value) const;

    static std::string resolveShaderPath(const std::string &logicalPath);

private:
    GLuint compileStage(GLenum type, const std::string &source, const std::string &name) const;
    // Compiles + links a fresh program WITHOUT touching `program_`, so a
    // failed hot reload leaves the previously working program running.
    [[nodiscard]] GLuint linkNewProgram(GLuint vert, GLuint frag) const;
    void reflectUniforms();
    GLint uniformLocation(const char *name) const;
    // Loads + compiles both stages into a candidate; on success installs it.
    // Returns false without throwing on errors (hot reload must not kill the
    // process mid-edit).
    [[nodiscard]] bool tryLoadProgram();

    struct SourceRef {
        std::string resolvedPath;
        std::filesystem::file_time_type lastWrite{};
    };
    SourceRef vertRef_, fragRef_;

    std::string name_;
    std::vector<std::string> defines_;
    std::string lastError_;
    GLuint program_ = 0;
    struct UniformHash {
        using is_transparent = void;
        size_t operator()(std::string_view name) const noexcept {
            return std::hash<std::string_view>{}(name);
        }
    };
    mutable std::unordered_map<std::string, GLint, UniformHash, std::equal_to<>> uniformCache_;
};

} // namespace horror
