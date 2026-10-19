#include "asset/GltfAsset.hpp"

#include "core/AssetPaths.hpp"
#include "core/Log.hpp"
#include "render/Mesh.hpp"
#include "render/Texture.hpp"

#include <glm/gtc/matrix_transform.hpp>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <unordered_map>

namespace horror {

// ============================================================================
// Minimal JSON reader for glTF "extras" and animation events.
// ============================================================================
namespace {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;
};

class JsonParser {
public:
    explicit JsonParser(const char *data) : p_(data ? data : "") {}

    bool parse(Json &out)
    {
        skipWs();
        if (!parseValue(out))
            return false;
        skipWs();
        return true;
    }

private:
    void skipWs()
    {
        while (*p_ && std::isspace(static_cast<unsigned char>(*p_)))
            ++p_;
    }

    bool parseString(std::string &out)
    {
        if (*p_ != '"')
            return false;
        ++p_;
        out.clear();
        while (*p_ && *p_ != '"') {
            if (*p_ == '\\') {
                ++p_;
                if (!*p_)
                    return false;
                switch (*p_) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    // Keep unicode escapes as-is; extras display only.
                    out += "\\u";
                    for (int i = 0; i < 4 && *(p_ + 1); ++i)
                        out += *++p_;
                    break;
                }
                default: out += *p_; break;
                }
                ++p_;
            } else {
                out += *p_++;
            }
        }
        if (*p_ != '"')
            return false;
        ++p_;
        return true;
    }

    bool parseNumber(Json &out)
    {
        const char *start = p_;
        if (*p_ == '-')
            ++p_;
        while (std::isdigit(static_cast<unsigned char>(*p_)) || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '+' || *p_ == '-')
            ++p_;
        if (p_ == start)
            return false;
        out.type = Json::Type::Number;
        out.number = std::strtod(std::string(start, p_).c_str(), nullptr);
        return true;
    }

    bool parseValue(Json &out)
    {
        skipWs();
        if (!*p_)
            return false;
        if (*p_ == '{') {
            ++p_;
            out.type = Json::Type::Object;
            skipWs();
            if (*p_ == '}') { ++p_; return true; }
            while (*p_) {
                std::string key;
                skipWs();
                if (!parseString(key))
                    return false;
                skipWs();
                if (*p_ != ':')
                    return false;
                ++p_;
                Json value;
                if (!parseValue(value))
                    return false;
                out.object.emplace_back(std::move(key), std::move(value));
                skipWs();
                if (*p_ == ',') { ++p_; continue; }
                if (*p_ == '}') { ++p_; return true; }
                return false;
            }
            return false;
        }
        if (*p_ == '[') {
            ++p_;
            out.type = Json::Type::Array;
            skipWs();
            if (*p_ == ']') { ++p_; return true; }
            while (*p_) {
                Json value;
                if (!parseValue(value))
                    return false;
                out.array.push_back(std::move(value));
                skipWs();
                if (*p_ == ',') { ++p_; continue; }
                if (*p_ == ']') { ++p_; return true; }
                return false;
            }
            return false;
        }
        if (*p_ == '"') {
            out.type = Json::Type::String;
            return parseString(out.text);
        }
        if (std::strncmp(p_, "true", 4) == 0) { p_ += 4; out.type = Json::Type::Bool; out.boolean = true; return true; }
        if (std::strncmp(p_, "false", 5) == 0) { p_ += 5; out.type = Json::Type::Bool; out.boolean = false; return true; }
        if (std::strncmp(p_, "null", 4) == 0) { p_ += 4; out.type = Json::Type::Null; return true; }
        return parseNumber(out);
    }

    const char *p_;
};

std::string jsonToString(const Json &v)
{
    switch (v.type) {
    case Json::Type::Null: return "null";
    case Json::Type::Bool: return v.boolean ? "true" : "false";
    case Json::Type::Number: {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", v.number);
        return buf;
    }
    case Json::Type::String: return v.text;
    case Json::Type::Array: {
        std::string s = "[";
        for (size_t i = 0; i < v.array.size(); ++i) {
            if (i) s += ",";
            s += jsonToString(v.array[i]);
        }
        return s + "]";
    }
    case Json::Type::Object: {
        std::string s = "{";
        for (size_t i = 0; i < v.object.size(); ++i) {
            if (i) s += ",";
            s += "\"" + v.object[i].first + "\":" + jsonToString(v.object[i].second);
        }
        return s + "}";
    }
    }
    return "null";
}

std::unordered_map<std::string, std::string> parseExtras(const cgltf_extras &extras)
{
    std::unordered_map<std::string, std::string> out;
    if (!extras.data || !*extras.data)
        return out;
    Json value;
    JsonParser parser(extras.data);
    if (!parser.parse(value) || value.type != Json::Type::Object)
        return out;
    for (const auto &[key, v] : value.object)
        out[key] = jsonToString(v);
    return out;
}

// Events live in the animation's extras:
//   "events": [ {"time": 0.35, "name": "footstep"}, ... ]
void parseAnimationEvents(const cgltf_extras &extras, std::vector<AnimationEvent> &out)
{
    if (!extras.data || !*extras.data)
        return;
    Json root;
    JsonParser parser(extras.data);
    if (!parser.parse(root) || root.type != Json::Type::Object)
        return;
    for (const auto &[key, value] : root.object) {
        if (key != "events" || value.type != Json::Type::Array)
            continue;
        for (const Json &e : value.array) {
            if (e.type != Json::Type::Object)
                continue;
            AnimationEvent ev;
            bool haveTime = false;
            for (const auto &[k, v] : e.object) {
                if (k == "time" && v.type == Json::Type::Number) {
                    ev.time = static_cast<float>(v.number);
                    haveTime = true;
                } else if (k == "name" && v.type == Json::Type::String) {
                    ev.name = v.text;
                }
            }
            if (haveTime && !ev.name.empty())
                out.push_back(std::move(ev));
        }
    }
    std::sort(out.begin(), out.end(),
        [](const AnimationEvent &a, const AnimationEvent &b) { return a.time < b.time; });
}

// ============================================================================
// Small helpers
// ============================================================================
int indexOf(const cgltf_node *base, const cgltf_node *node)
{
    if (!node)
        return -1;
    return static_cast<int>(node - base);
}

int indexOf(const cgltf_mesh *base, const cgltf_mesh *mesh)
{
    if (!mesh)
        return -1;
    return static_cast<int>(mesh - base);
}

int indexOf(const cgltf_material *base, const cgltf_material *material)
{
    if (!material)
        return -1;
    return static_cast<int>(material - base);
}

const cgltf_attribute *findAttribute(const cgltf_primitive &prim, cgltf_attribute_type type, int set = 0)
{
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        const cgltf_attribute &a = prim.attributes[i];
        if (a.type == type && a.index == set)
            return &a;
    }
    return nullptr;
}

std::string safeName(const char *name, const std::string &fallback)
{
    return (name && *name) ? std::string(name) : fallback;
}

bool decodeBase64(const std::string &in, std::vector<uint8_t> &out)
{
    static const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int values[256];
    std::fill(std::begin(values), std::end(values), -1);
    for (int i = 0; i < 64; ++i)
        values[static_cast<unsigned char>(table[i])] = i;

    out.clear();
    out.reserve(in.size() * 3 / 4);
    int bits = 0, accumulated = 0;
    for (unsigned char c : in) {
        if (c == '=')
            break;
        const int v = values[c];
        if (v < 0)
            continue;
        accumulated = (accumulated << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulated >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

// Generate smooth normals and a tangent basis when the asset omits them.
void generateNormals(std::vector<Vertex> &verts, const std::vector<uint32_t> &indices)
{
    for (auto &v : verts)
        v.normal = glm::vec3(0.0f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        Vertex &a = verts[indices[i]];
        Vertex &b = verts[indices[i + 1]];
        Vertex &c = verts[indices[i + 2]];
        const glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (auto &v : verts) {
        const float len = glm::length(v.normal);
        v.normal = len > 1e-8f ? v.normal / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

void generateTangents(std::vector<Vertex> &verts, const std::vector<uint32_t> &indices)
{
    std::vector<glm::vec3> tangents(verts.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(verts.size(), glm::vec3(0.0f));

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        Vertex &a = verts[indices[i]];
        Vertex &b = verts[indices[i + 1]];
        Vertex &c = verts[indices[i + 2]];
        const glm::vec3 e1 = b.position - a.position;
        const glm::vec3 e2 = c.position - a.position;
        const glm::vec2 duv1 = b.uv - a.uv;
        const glm::vec2 duv2 = c.uv - a.uv;
        const float det = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(det) < 1e-8f)
            continue;
        const float r = 1.0f / det;
        const glm::vec3 t = (e1 * duv2.y - e2 * duv1.y) * r;
        const glm::vec3 bt = (e2 * duv1.x - e1 * duv2.x) * r;
        tangents[indices[i]] += t;
        tangents[indices[i + 1]] += t;
        tangents[indices[i + 2]] += t;
        bitangents[indices[i]] += bt;
        bitangents[indices[i + 1]] += bt;
        bitangents[indices[i + 2]] += bt;
    }

    for (size_t i = 0; i < verts.size(); ++i) {
        const glm::vec3 n = verts[i].normal;
        glm::vec3 t = tangents[i] - n * glm::dot(n, tangents[i]);
        const float len = glm::length(t);
        if (len < 1e-8f) {
            // Degenerate UVs: any vector orthogonal to the normal will do.
            const glm::vec3 helper = std::abs(n.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            t = glm::normalize(glm::cross(helper, n));
        } else {
            t /= len;
        }
        const float handedness = glm::dot(glm::cross(n, t), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        verts[i].tangent = glm::vec4(t, handedness);
    }
}

} // namespace

// ============================================================================
// GltfNode
// ============================================================================
glm::mat4 GltfNode::localMatrix() const
{
    return glm::translate(glm::mat4(1.0f), translation)
        * glm::mat4_cast(rotation)
        * glm::scale(glm::mat4(1.0f), scale);
}

// ============================================================================
// GltfModel
// ============================================================================
GltfModel::GltfModel(std::string path)
    : path_(std::move(path))
    , clips_(std::make_shared<std::vector<std::shared_ptr<const AnimationClip>>>())
{
    load();
}

GltfModel::~GltfModel() = default;

glm::mat4 GltfModel::restNodeWorldMatrix(int node) const
{
    glm::mat4 world(1.0f);
    int guard = 0;
    while (node >= 0 && static_cast<size_t>(node) < nodes_.size() && guard++ < 256) {
        world = nodes_[static_cast<size_t>(node)].localMatrix() * world;
        node = nodes_[static_cast<size_t>(node)].parent;
    }
    return world;
}

const AnimationClip *GltfModel::findClip(const std::string &name) const
{
    for (const auto &clip : *clips_)
        if (clip && clip->name == name)
            return clip.get();
    return nullptr;
}

int GltfModel::findNode(const std::string &name) const
{
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].name == name)
            return static_cast<int>(i);
    return -1;
}

void GltfModel::load()
{
    const std::filesystem::path resolved = resolveAssetPath(path_);
    resolvedPath_ = resolved.string();
    if (!std::filesystem::exists(resolved)) {
        error_ = "file not found: " + resolved.string();
        HOR_LOG_ERROR("glTF load failed: {}", error_);
        return;
    }

    cgltf_options options{};
    cgltf_data *data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, resolvedPath_.c_str(), &data);
    if (result != cgltf_result_success || !data) {
        error_ = "cgltf_parse_file failed (" + std::to_string(static_cast<int>(result)) + ")";
        HOR_LOG_ERROR("glTF load failed: {} [{}]", error_, resolvedPath_);
        return;
    }

    const std::filesystem::path baseDir = assetDirectory(resolvedPath_);
    result = cgltf_load_buffers(&options, data, baseDir.string().c_str());
    if (result != cgltf_result_success) {
        error_ = "cgltf_load_buffers failed";
        cgltf_free(data);
        HOR_LOG_ERROR("glTF load failed: {} [{}]", error_, resolvedPath_);
        return;
    }

    // ---- materials + textures ---------------------------------------------
    // Deduplicate textures: several materials commonly share one image.
    std::unordered_map<const cgltf_texture *, std::shared_ptr<Texture>> textureCache;

    auto loadTexture = [&](const cgltf_texture *tex, bool srgb) -> std::shared_ptr<Texture> {
        if (!tex || !tex->image)
            return nullptr;
        auto it = textureCache.find(tex);
        if (it != textureCache.end())
            return it->second;

        Texture::Params params;
        params.srgb = srgb;
        std::shared_ptr<Texture> texture;
        const cgltf_image *image = tex->image;
        if (image->buffer_view) {
            const void *bytes = cgltf_buffer_view_data(image->buffer_view);
            texture = std::make_shared<Texture>(
                static_cast<const uint8_t *>(bytes), image->buffer_view->size, params);
        } else if (image->uri) {
            const std::string uri = image->uri;
            if (uri.rfind("data:", 0) == 0) {
                const size_t comma = uri.find(',');
                std::vector<uint8_t> decoded;
                if (comma != std::string::npos && decodeBase64(uri.substr(comma + 1), decoded))
                    texture = std::make_shared<Texture>(decoded.data(), decoded.size(), params);
                else
                    HOR_LOG_WARN("glTF: undecodable data-URI image '{}'", image->name ? image->name : "");
            } else {
                const std::string imagePath = (baseDir / uri).string();
                texture = std::make_shared<Texture>(imagePath, params);
            }
        }
        if (!texture)
            return nullptr;
        textures_.push_back(texture);
        textureCache[tex] = texture;
        return texture;
    };

    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material &src = data->materials[i];
        auto mat = std::make_shared<Material>();
        mat->name = safeName(src.name, "material" + std::to_string(i));

        if (src.has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness &pbr = src.pbr_metallic_roughness;
            mat->albedo = glm::vec3(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]);
            mat->metallic = pbr.metallic_factor;
            mat->roughness = std::clamp(pbr.roughness_factor, 0.04f, 1.0f);
            if (pbr.base_color_texture.texture) {
                if (pbr.base_color_texture.texcoord != 0)
                    HOR_LOG_WARN_ONCE("glTF material '{}' uses TEXCOORD_{}; only uv0 is supported",
                        mat->name, pbr.base_color_texture.texcoord);
                mat->baseColorTex = loadTexture(pbr.base_color_texture.texture, true).get();
            }
            if (pbr.metallic_roughness_texture.texture)
                mat->metallicRoughnessTex =
                    loadTexture(pbr.metallic_roughness_texture.texture, false).get();
        }
        if (src.normal_texture.texture)
            mat->normalTex = loadTexture(src.normal_texture.texture, false).get();

        // Emissive: the forward shader has a scalar strength; keep the color
        // for a future PBR pass and drive strength from the brightest channel.
        mat->emissiveColor = glm::vec3(src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2]);
        mat->emissiveStrength = std::max({ mat->emissiveColor.r, mat->emissiveColor.g, mat->emissiveColor.b });
        // Alpha modes other than OPAQUE need sorting/blending support; the
        // loader keeps the material opaque rather than rendering it wrong.
        mat->opacity = 1.0f;

        materials_.push_back(std::move(mat));
    }
    // Meshes without a material share one default material at the end.
    {
        auto fallback = std::make_shared<Material>();
        fallback->name = "glTF default";
        fallback->albedo = glm::vec3(0.75f);
        fallback->roughness = 0.9f;
        materials_.push_back(std::move(fallback));
    }
    const int defaultMaterial = static_cast<int>(materials_.size()) - 1;

    // ---- meshes ------------------------------------------------------------
    boundsMin_ = glm::vec3(std::numeric_limits<float>::max());
    boundsMax_ = glm::vec3(-std::numeric_limits<float>::max());
    bool haveBounds = false;

    auto readVec3 = [](const cgltf_accessor *acc, cgltf_size i) {
        cgltf_float v[3] = { 0, 0, 0 };
        cgltf_accessor_read_float(acc, i, v, 3);
        return glm::vec3(v[0], v[1], v[2]);
    };

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh &srcMesh = data->meshes[mi];
        GltfMesh mesh;
        mesh.name = safeName(srcMesh.name, "mesh" + std::to_string(mi));
        mesh.boundsMin = glm::vec3(std::numeric_limits<float>::max());
        mesh.boundsMax = glm::vec3(-std::numeric_limits<float>::max());

        for (cgltf_size pi = 0; pi < srcMesh.primitives_count; ++pi) {
            const cgltf_primitive &prim = srcMesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) {
                HOR_LOG_WARN("glTF mesh '{}' primitive {} is not triangles; skipped", mesh.name, pi);
                continue;
            }
            const cgltf_attribute *posAttr = findAttribute(prim, cgltf_attribute_type_position);
            if (!posAttr || !posAttr->data) {
                HOR_LOG_WARN("glTF mesh '{}' primitive {} has no POSITION; skipped", mesh.name, pi);
                continue;
            }

            const cgltf_accessor *pos = posAttr->data;
            const cgltf_accessor *nrm = nullptr;
            const cgltf_accessor *uv = nullptr;
            const cgltf_accessor *tan = nullptr;
            const cgltf_accessor *joints = nullptr;
            const cgltf_accessor *weights = nullptr;
            if (const auto *a = findAttribute(prim, cgltf_attribute_type_normal))
                nrm = a->data;
            if (const auto *a = findAttribute(prim, cgltf_attribute_type_texcoord))
                uv = a->data;
            if (const auto *a = findAttribute(prim, cgltf_attribute_type_tangent))
                tan = a->data;
            if (const auto *a = findAttribute(prim, cgltf_attribute_type_joints))
                joints = a->data;
            if (const auto *a = findAttribute(prim, cgltf_attribute_type_weights))
                weights = a->data;

            Mesh::Geometry geometry;
            geometry.vertices.resize(pos->count);
            for (cgltf_size v = 0; v < pos->count; ++v) {
                Vertex &vertex = geometry.vertices[v];
                vertex.position = readVec3(pos, v);
                if (nrm)
                    vertex.normal = readVec3(nrm, v);
                if (uv) {
                    cgltf_float t[2] = { 0, 0 };
                    cgltf_accessor_read_float(uv, v, t, 2);
                    vertex.uv = glm::vec2(t[0], t[1]);
                }
                if (tan) {
                    cgltf_float t[4] = { 1, 0, 0, 1 };
                    cgltf_accessor_read_float(tan, v, t, 4);
                    vertex.tangent = glm::vec4(t[0], t[1], t[2], t[3]);
                }
                if (joints) {
                    cgltf_uint j[4] = { 0, 0, 0, 0 };
                    cgltf_accessor_read_uint(joints, v, j, 4);
                    vertex.joints = glm::uvec4(j[0], j[1], j[2], j[3]);
                }
                if (weights) {
                    cgltf_float w[4] = { 0, 0, 0, 0 };
                    cgltf_accessor_read_float(weights, v, w, 4);
                    glm::vec4 weights4(w[0], w[1], w[2], w[3]);
                    const float sum = weights4.x + weights4.y + weights4.z + weights4.w;
                    vertex.weights = sum > 1e-6f ? weights4 / sum : glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                }
            }

            // Indices: absent means sequential.
            if (prim.indices) {
                geometry.indices.resize(prim.indices->count);
                for (cgltf_size k = 0; k < prim.indices->count; ++k) {
                    cgltf_uint index = 0;
                    cgltf_accessor_read_uint(prim.indices, k, &index, 1);
                    geometry.indices[k] = index;
                }
            } else {
                geometry.indices.resize(pos->count);
                for (cgltf_size k = 0; k < pos->count; ++k)
                    geometry.indices[k] = static_cast<uint32_t>(k);
            }

            if (!nrm)
                generateNormals(geometry.vertices, geometry.indices);
            if (!tan)
                generateTangents(geometry.vertices, geometry.indices);

            auto gpuMesh = std::make_shared<Mesh>(std::move(geometry),
                mesh.name + "[" + std::to_string(pi) + "]");
            mesh.boundsMin = glm::min(mesh.boundsMin, gpuMesh->minBounds());
            mesh.boundsMax = glm::max(mesh.boundsMax, gpuMesh->maxBounds());
            if (gpuMesh->indexCount() > 0) {
                haveBounds = true;
                boundsMin_ = glm::min(boundsMin_, gpuMesh->minBounds());
                boundsMax_ = glm::max(boundsMax_, gpuMesh->maxBounds());
            }

            GltfMeshPart part;
            part.mesh = std::move(gpuMesh);
            part.material = indexOf(data->materials, prim.material);
            if (part.material < 0)
                part.material = defaultMaterial;
            mesh.parts.push_back(std::move(part));
        }
        meshes_.push_back(std::move(mesh));
    }
    if (!haveBounds) {
        boundsMin_ = glm::vec3(0.0f);
        boundsMax_ = glm::vec3(0.0f);
    }

    // ---- nodes -------------------------------------------------------------
    nodes_.resize(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node &src = data->nodes[i];
        GltfNode &node = nodes_[i];
        node.name = safeName(src.name, "node" + std::to_string(i));
        node.parent = indexOf(data->nodes, src.parent);
        if (src.has_matrix) {
            // glTF matrices are column-major, matching glm. Decompose to TRS
            // (no shear support, which is fine for Blender exports).
            glm::mat4 m(1.0f);
            std::memcpy(&m[0][0], src.matrix, sizeof(float) * 16);
            const glm::vec3 c0(m[0]);
            const glm::vec3 c1(m[1]);
            const glm::vec3 c2(m[2]);
            node.scale = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));
            glm::mat3 basis(1.0f);
            if (node.scale.x > 1e-8f)
                basis[0] = c0 / node.scale.x;
            if (node.scale.y > 1e-8f)
                basis[1] = c1 / node.scale.y;
            if (node.scale.z > 1e-8f)
                basis[2] = c2 / node.scale.z;
            if (glm::dot(glm::cross(basis[0], basis[1]), basis[2]) < 0.0f) {
                node.scale.z = -node.scale.z;
                basis[2] = -basis[2];
            }
            node.rotation = glm::normalize(glm::quat_cast(basis));
            node.translation = glm::vec3(m[3]);
        } else {
            if (src.has_translation)
                node.translation = glm::vec3(src.translation[0], src.translation[1], src.translation[2]);
            if (src.has_rotation)
                node.rotation = glm::quat(src.rotation[3], src.rotation[0], src.rotation[1], src.rotation[2]);
            if (src.has_scale)
                node.scale = glm::vec3(src.scale[0], src.scale[1], src.scale[2]);
        }
        node.mesh = indexOf(data->meshes, src.mesh);
        node.skin = src.skin ? static_cast<int>(src.skin - data->skins) : -1;
        node.light = src.light ? static_cast<int>(src.light - data->lights) : -1;
        node.extras = parseExtras(src.extras);
    }
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const int parent = nodes_[i].parent;
        if (parent >= 0)
            nodes_[static_cast<size_t>(parent)].children.push_back(static_cast<int>(i));
    }

    if (data->scene && data->scene->nodes_count > 0) {
        for (cgltf_size i = 0; i < data->scene->nodes_count; ++i)
            roots_.push_back(indexOf(data->nodes, data->scene->nodes[i]));
    } else {
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].parent < 0)
                roots_.push_back(static_cast<int>(i));
    }
    roots_.erase(std::remove(roots_.begin(), roots_.end(), -1), roots_.end());

    // ---- lights ------------------------------------------------------------
    for (cgltf_size i = 0; i < data->lights_count; ++i) {
        const cgltf_light &src = data->lights[i];
        GltfLight light;
        light.name = safeName(src.name, "light" + std::to_string(i));
        light.color = glm::vec3(src.color[0], src.color[1], src.color[2]);
        light.intensity = src.intensity;
        light.range = src.range;
        switch (src.type) {
        case cgltf_light_type_directional: light.type = GltfLight::Type::Directional; break;
        case cgltf_light_type_spot: light.type = GltfLight::Type::Spot; break;
        case cgltf_light_type_point:
        default: light.type = GltfLight::Type::Point; break;
        }
        if (light.type == GltfLight::Type::Spot) {
            // glTF cone angles are half-angles in radians.
            light.innerConeDeg = glm::degrees(src.spot_inner_cone_angle);
            light.outerConeDeg = glm::degrees(src.spot_outer_cone_angle);
        }
        lights_.push_back(std::move(light));
    }

    // ---- skins -------------------------------------------------------------
    for (cgltf_size si = 0; si < data->skins_count; ++si) {
        const cgltf_skin &src = data->skins[si];
        auto skeleton = std::make_shared<Skeleton>();
        skeleton->name = safeName(src.name, "skin" + std::to_string(si));

        // Copy the whole node hierarchy: animation channels index into it.
        skeleton->nodes.resize(nodes_.size());
        for (size_t i = 0; i < nodes_.size(); ++i) {
            skeleton->nodes[i].parent = nodes_[i].parent;
            skeleton->nodes[i].translation = nodes_[i].translation;
            skeleton->nodes[i].rotation = nodes_[i].rotation;
            skeleton->nodes[i].scale = nodes_[i].scale;
        }

        skeleton->joints.resize(src.joints_count);
        for (cgltf_size j = 0; j < src.joints_count; ++j)
            skeleton->joints[j] = indexOf(data->nodes, src.joints[j]);

        skeleton->inverseBind.resize(src.joints_count, glm::mat4(1.0f));
        if (src.inverse_bind_matrices) {
            const cgltf_accessor *acc = src.inverse_bind_matrices;
            std::vector<float> values(acc->count * 16);
            cgltf_accessor_unpack_floats(acc, values.data(), values.size());
            for (cgltf_size j = 0; j < acc->count && j < src.joints_count; ++j)
                std::memcpy(&skeleton->inverseBind[j][0][0], &values[j * 16], sizeof(float) * 16);
        }

        // glTF skinning is defined in the *mesh node's* space:
        //   jointMatrix = inverse(meshNodeGlobal) * jointGlobal * inverseBind
        // Several samples (CesiumMan) put the mesh under Z-up conversion
        // nodes, so keep the inverse around and apply it at runtime ahead of
        // the joint world (pre-multiplying it into the bind matrices would
        // change the operand order and shear the skin).
        int meshNode = -1;
        for (size_t n = 0; n < nodes_.size(); ++n) {
            if (nodes_[n].skin == static_cast<int>(si)) {
                meshNode = static_cast<int>(n);
                break;
            }
        }
        if (meshNode >= 0) {
            skeleton->meshNodeInverse = glm::inverse(restNodeWorldMatrix(meshNode));
        } else {
            HOR_LOG_WARN("glTF skin '{}' is not referenced by any node; using raw bind matrices",
                skeleton->name);
        }

        // Joint -> joint parent, for bone debug drawing.
        skeleton->jointParents.assign(src.joints_count, -1);
        for (cgltf_size j = 0; j < src.joints_count; ++j) {
            const int node = skeleton->joints[j];
            const int parentNode = node >= 0 ? nodes_[static_cast<size_t>(node)].parent : -1;
            if (parentNode < 0)
                continue;
            for (cgltf_size k = 0; k < src.joints_count; ++k) {
                if (skeleton->joints[k] == parentNode) {
                    skeleton->jointParents[j] = static_cast<int>(k);
                    break;
                }
            }
        }
        skeletons_.push_back(std::move(skeleton));
    }

    // ---- animations --------------------------------------------------------
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation &src = data->animations[ai];
        auto clip = std::make_shared<AnimationClip>();
        clip->name = safeName(src.name, "clip" + std::to_string(ai));

        for (cgltf_size ci = 0; ci < src.channels_count; ++ci) {
            const cgltf_animation_channel &channel = src.channels[ci];
            if (!channel.target_node || !channel.sampler || !channel.sampler->input || !channel.sampler->output)
                continue;

            AnimationChannel out;
            out.node = indexOf(data->nodes, channel.target_node);
            switch (channel.target_path) {
            case cgltf_animation_path_type_translation: out.path = AnimationChannel::Path::Translation; break;
            case cgltf_animation_path_type_rotation: out.path = AnimationChannel::Path::Rotation; break;
            case cgltf_animation_path_type_scale: out.path = AnimationChannel::Path::Scale; break;
            default: continue; // morph target weights are not supported
            }
            switch (channel.sampler->interpolation) {
            case cgltf_interpolation_type_step: out.interpolation = AnimationInterpolation::Step; break;
            case cgltf_interpolation_type_cubic_spline: out.interpolation = AnimationInterpolation::CubicSpline; break;
            case cgltf_interpolation_type_linear:
            default: out.interpolation = AnimationInterpolation::Linear; break;
            }

            const cgltf_accessor *input = channel.sampler->input;
            const cgltf_accessor *output = channel.sampler->output;
            out.times.resize(input->count);
            cgltf_accessor_unpack_floats(input, out.times.data(), out.times.size());

            const int components = out.path == AnimationChannel::Path::Rotation ? 4 : 3;
            std::vector<float> raw(output->count * components);
            cgltf_accessor_unpack_floats(output, raw.data(), raw.size());
            out.values.resize(output->count);
            for (cgltf_size k = 0; k < output->count; ++k) {
                glm::vec4 v(0.0f);
                v.x = raw[k * components + 0];
                v.y = raw[k * components + 1];
                v.z = raw[k * components + 2];
                if (components == 4)
                    v.w = raw[k * components + 3];
                out.values[k] = v;
            }
            if (!out.times.empty())
                clip->duration = std::max(clip->duration, out.times.back());
            clip->channels.push_back(std::move(out));
        }
        parseAnimationEvents(src.extras, clip->events);
        clips_->push_back(std::move(clip));
    }

    valid_ = true;
    cgltf_free(data);

    HOR_LOG_INFO("glTF loaded: '{}' | {} nodes, {} meshes, {} materials, {} skins, {} clips, {} lights",
        path_, nodes_.size(), meshes_.size(), materials_.size(), skeletons_.size(),
        clips_->size(), lights_.size());
}

// ============================================================================
// AssetLibrary
// ============================================================================
std::shared_ptr<GltfModel> AssetLibrary::loadModel(const std::string &relativePath)
{
    auto it = models_.find(relativePath);
    if (it != models_.end()) {
        if (auto existing = it->second.lock())
            return existing;
    }
    auto model = std::make_shared<GltfModel>(relativePath);
    models_[relativePath] = model;
    return model;
}

void AssetLibrary::clear()
{
    models_.clear();
}

size_t AssetLibrary::liveCount() const
{
    size_t count = 0;
    for (const auto &[key, weak] : models_)
        if (!weak.expired())
            ++count;
    return count;
}

} // namespace horror
