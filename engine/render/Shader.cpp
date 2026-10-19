#include "render/Shader.hpp"

#include "core/Log.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace horror {

namespace {
// Resolve order: (1) cwd-relative -- the runtime copy CMake places next to the
// executable; (2) fallback baked at configure time so running from an IDE/any
// cwd still finds the repo copy. Keeps "edit shader, see change" robust.
#ifndef HOR_SHADER_SRC_DIR
#define HOR_SHADER_SRC_DIR "../shaders"
#endif

bool readFile(const std::string &path, std::string &out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    return true;
}

std::filesystem::file_time_type queryMtime(const std::string &path)
{
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type::min() : t;
}

// #version must stay on the first line, so defines go right after it.
std::string injectDefines(std::string source, const std::vector<std::string> &defines)
{
    if (defines.empty())
        return source;
    std::string block;
    block.reserve(defines.size() * 24);
    for (const std::string &d : defines)
        block += "#define " + d + "\n";
    const size_t firstNewline = source.find('\n');
    if (firstNewline == std::string::npos)
        return block + source;
    source.insert(firstNewline + 1, block);
    return source;
}
} // namespace

Shader::Shader(const std::string &vertexPath, const std::string &fragmentPath,
    const std::string &name, const std::vector<std::string> &defines)
    : name_(name)
    , defines_(defines)
{
    vertRef_.resolvedPath = resolveShaderPath(vertexPath);
    fragRef_.resolvedPath = resolveShaderPath(fragmentPath);
    if (name_.empty())
        name_ = fragRef_.resolvedPath;

    if (!tryLoadProgram())
        throw std::runtime_error("shader build failed: " + lastError_);

    reflectUniforms();
    vertRef_.lastWrite = queryMtime(vertRef_.resolvedPath);
    fragRef_.lastWrite = queryMtime(fragRef_.resolvedPath);
}

Shader::~Shader()
{
    if (program_)
        glDeleteProgram(program_);
}

Shader::Shader(Shader &&o) noexcept
    : vertRef_(std::move(o.vertRef_))
    , fragRef_(std::move(o.fragRef_))
    , name_(std::move(o.name_))
    , defines_(std::move(o.defines_))
    , lastError_(std::move(o.lastError_))
    , program_(o.program_)
    , uniformCache_(std::move(o.uniformCache_))
{
    o.program_ = 0;
}

Shader &Shader::operator=(Shader &&o) noexcept
{
    if (this == &o)
        return *this;
    if (program_)
        glDeleteProgram(program_);
    vertRef_ = std::move(o.vertRef_);
    fragRef_ = std::move(o.fragRef_);
    name_ = std::move(o.name_);
    defines_ = std::move(o.defines_);
    lastError_ = std::move(o.lastError_);
    program_ = o.program_;
    uniformCache_ = std::move(o.uniformCache_);
    o.program_ = 0;
    return *this;
}

std::string Shader::resolveShaderPath(const std::string &logicalPath)
{
    std::filesystem::path cwdPath("shaders");
    cwdPath /= logicalPath;
    std::filesystem::path srcPath(HOR_SHADER_SRC_DIR);
    srcPath /= logicalPath;

    if (std::filesystem::exists(cwdPath))
        return cwdPath.string();
    if (std::filesystem::exists(srcPath))
        return srcPath.string();
    return cwdPath.string();
}

GLuint Shader::compileStage(GLenum type, const std::string &source, const std::string &stageName) const
{
    GLuint shader = glCreateShader(type);
    const char *src = source.c_str();
    GLint len = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &src, &len);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char infoLog[1024];
        GLsizei written = 0;
        glGetShaderInfoLog(shader, static_cast<GLsizei>(sizeof(infoLog)), &written, infoLog);
        HOR_LOG_ERROR("shader compile failed [{}]: {}", stageName, std::string(infoLog, written));
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Compiles + links a *fresh* program without touching the current one, so a
// failed hot reload leaves the old program running. Returns 0 on failure.
GLuint Shader::linkNewProgram(GLuint vert, GLuint frag) const
{
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char infoLog[1024];
        GLsizei written = 0;
        glGetProgramInfoLog(prog, static_cast<GLsizei>(sizeof(infoLog)), &written, infoLog);
        HOR_LOG_ERROR("shader link failed [{}]: {}", name_, std::string(infoLog, written));
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool Shader::tryLoadProgram()
{
    std::string vertSrc, fragSrc;
    if (!readFile(vertRef_.resolvedPath, vertSrc)) {
        lastError_ = "cannot read vertex shader: " + vertRef_.resolvedPath;
        HOR_LOG_ERROR("{}", lastError_);
        return false;
    }
    if (!readFile(fragRef_.resolvedPath, fragSrc)) {
        lastError_ = "cannot read fragment shader: " + fragRef_.resolvedPath;
        HOR_LOG_ERROR("{}", lastError_);
        return false;
    }
    vertSrc = injectDefines(std::move(vertSrc), defines_);
    fragSrc = injectDefines(std::move(fragSrc), defines_);

    GLuint vert = compileStage(GL_VERTEX_SHADER, vertSrc, name_ + ":vert");
    if (!vert)
        return false;
    GLuint frag = compileStage(GL_FRAGMENT_SHADER, fragSrc, name_ + ":frag");
    if (!frag) {
        glDeleteShader(vert);
        return false;
    }

    GLuint candidate = linkNewProgram(vert, frag);
    if (!candidate)
        return false;

    if (program_)
        glDeleteProgram(program_);
    program_ = candidate;
    lastError_.clear();
    return true;
}

void Shader::reflectUniforms()
{
    uniformCache_.clear();
    if (!program_)
        return;
    GLint count = 0;
    glGetProgramiv(program_, GL_ACTIVE_UNIFORMS, &count);
    char nameBuffer[256];
    for (GLint i = 0; i < count; ++i) {
        GLenum type = 0;
        GLsizei length = 0, size = 0;
        glGetActiveUniform(program_, static_cast<GLuint>(i),
            static_cast<GLsizei>(sizeof(nameBuffer)), &length, &size, &type, nameBuffer);
        if (length <= 0)
            continue;
        uniformCache_[nameBuffer] = glGetUniformLocation(program_, nameBuffer);
    }
}

bool Shader::checkHotReload()
{
    auto vt = queryMtime(vertRef_.resolvedPath);
    auto ft = queryMtime(fragRef_.resolvedPath);
    bool changed = (vt != vertRef_.lastWrite) || (ft != fragRef_.lastWrite);
    if (!changed || vertRef_.resolvedPath.empty())
        return false;

    // Editors occasionally truncate a file mid-save; if we can't read either
    // stage right now, skip this cycle and retry next frame.
    std::error_code ec;
    std::filesystem::file_time_type probe = std::filesystem::last_write_time(vertRef_.resolvedPath, ec);
    if (ec)
        return false;

    bool ok = tryLoadProgram();
    vertRef_.lastWrite = vt;
    fragRef_.lastWrite = ft;
    if (ok) {
        reflectUniforms();
        HOR_LOG_INFO("shader reloaded [{}]", name_);
    }
    return ok;
}

void Shader::bind() const
{
    HOR_CHECK(program_ != 0);
    glUseProgram(program_);
}

GLint Shader::uniformLocation(const std::string &name) const
{
    auto it = uniformCache_.find(name);
    if (it != uniformCache_.end())
        return it->second;
    GLint loc = program_ ? glGetUniformLocation(program_, name.c_str()) : -1;
    if (loc < 0)
        HOR_LOG_WARN_ONCE("uniform '{}' not found in shader [{}] (optimized out?)", name, name_);
    uniformCache_[name] = loc; // caches -1 too: avoids re-querying per call
    return loc;
}

// GL 4.5 DSA uniform setters: glProgramUniform* targets the program directly,
// no glUseProgram needed before setting. Keeps call sites terse.

void Shader::setUniform(const char *name, float value) const
{
    glProgramUniform1f(program_, uniformLocation(name), value);
}

void Shader::setUniform(const char *name, int value) const
{
    glProgramUniform1i(program_, uniformLocation(name), value);
}

void Shader::setUniform(const char *name, const glm::vec2 &v) const
{
    glProgramUniform2fv(program_, uniformLocation(name), 1, &v[0]);
}

void Shader::setUniform(const char *name, const glm::vec3 &v) const
{
    glProgramUniform3fv(program_, uniformLocation(name), 1, &v[0]);
}

void Shader::setUniform(const char *name, const glm::vec4 &v) const
{
    glProgramUniform4fv(program_, uniformLocation(name), 1, &v[0]);
}

void Shader::setUniform(const char *name, const glm::mat3 &m) const
{
    glProgramUniformMatrix3fv(program_, uniformLocation(name), 1, GL_FALSE, &m[0][0]);
}

void Shader::setUniform(const char *name, const glm::mat4 &m) const
{
    glProgramUniformMatrix4fv(program_, uniformLocation(name), 1, GL_FALSE, &m[0][0]);
}

} // namespace horror
