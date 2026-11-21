// Solid-mode map viewport embedded in the Windows editor.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string_view>
#include <vector>

#define API extern "C" __declspec(dllexport)
namespace {
using glm::vec3;
struct Part {
    GLuint list;
    int node;
    vec3 color, lo, hi;
};
struct Model {
    std::vector<Part> parts;
    ~Model()
    {
        for (const auto& p : parts)
            glDeleteLists(p.list, 1);
    }
};
struct Hit {
    int object, node, handle, note;
};
std::vector<Hit> hits;
HWND window;
HDC dc;
HGLRC context;
GLuint box;
glm::mat4 view, projection;
glm::ivec4 viewport;
bool picking;

void vertex(vec3 p) { glVertex3fv(glm::value_ptr(p)); }
void paint(vec3 color) { glColor3fv(glm::value_ptr(color)); }
void color(Hit hit, vec3 visible)
{
    if (!picking) {
        paint(visible);
        return;
    }
    hits.push_back(hit);
    const auto id = hits.size();
    glColor3ub(id & 255, (id >> 8) & 255, (id >> 16) & 255);
}
void buildBox()
{
    box = glGenLists(1);
    glNewList(box, GL_COMPILE);
    glBegin(GL_QUADS);
    for (int axis = 0; axis < 3; axis++)
        for (float sign : { -1.f, 1.f }) {
            vec3 normal(0);
            normal[axis] = sign;
            glNormal3fv(glm::value_ptr(normal));
            for (auto corner : std::array<glm::vec2, 4> { { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } } }) {
                vec3 p = normal * .5f;
                p[(axis + 1) % 3] = corner.x * sign * .5f;
                p[(axis + 2) % 3] = corner.y * .5f;
                vertex(p);
            }
        }
    glEnd();
    glEndList();
}

void meshNode(cgltf_data* data, cgltf_node* node, Model& model)
{
    float raw[16];
    cgltf_node_transform_world(node, raw);
    const auto matrix = glm::make_mat4(raw);
    if (node->mesh)
        for (size_t i = 0; i < node->mesh->primitives_count; i++) {
            const auto& p = node->mesh->primitives[i];
            const auto* pos = cgltf_find_accessor(&p, cgltf_attribute_type_position, 0);
            if (!pos || pos->type != cgltf_type_vec3 || !pos->count)
                continue;
            std::vector<float> positions(pos->count * 3);
            if (cgltf_accessor_unpack_floats(pos, positions.data(), positions.size()) != positions.size())
                continue;
            std::vector<vec3> points(pos->count);
            vec3 lo(1e30f), hi(-1e30f);
            for (size_t j = 0; j < points.size(); ++j) {
                points[j] = matrix * glm::vec4(glm::make_vec3(&positions[3 * j]), 1);
                lo = glm::min(lo, points[j]);
                hi = glm::max(hi, points[j]);
            }
            std::vector<unsigned> indices(p.indices ? p.indices->count : pos->count);
            if (p.indices) {
                if (cgltf_accessor_unpack_indices(p.indices, indices.data(), sizeof(unsigned), indices.size())
                    != indices.size())
                    continue;
            } else
                for (size_t j = 0; j < indices.size(); ++j)
                    indices[j] = static_cast<unsigned>(j);
            constexpr GLenum modes[] = { GL_POINTS, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES,
                GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN };
            int mode = int(p.type) - int(cgltf_primitive_type_points);
            if (mode < 0 || mode >= 7)
                continue;
            const auto* norm = cgltf_find_accessor(&p, cgltf_attribute_type_normal, 0);
            std::vector<float> normals;
            if (norm && norm->type == cgltf_type_vec3 && norm->count == pos->count) {
                normals.resize(positions.size());
                if (cgltf_accessor_unpack_floats(norm, normals.data(), normals.size()) != normals.size())
                    normals.clear();
            }
            const auto normalMatrix = glm::transpose(glm::inverse(glm::mat3(matrix)));
            GLuint list = glGenLists(1);
            glNewList(list, GL_COMPILE);
            glBegin(modes[mode]);
            glNormal3f(0, 1, 0);
            for (size_t j = 0; j < indices.size(); ++j) {
                auto idx = indices[j];
                if (idx >= points.size())
                    continue;
                if (!normals.empty()) {
                    auto n = normalMatrix * glm::make_vec3(&normals[3 * idx]);
                    glNormal3fv(glm::value_ptr(n));
                } else if (modes[mode] == GL_TRIANGLES && j % 3 == 0 && j + 2 < indices.size()
                    && indices[j + 1] < points.size() && indices[j + 2] < points.size()) {
                    auto n = glm::cross(
                        points[indices[j + 1]] - points[idx], points[indices[j + 2]] - points[idx]);
                    glNormal3fv(glm::value_ptr(n));
                }
                vertex(points[idx]);
            }
            glEnd();
            glEndList();
            vec3 tint(.65f, .70f, .73f);
            if (p.material && p.material->has_pbr_metallic_roughness)
                tint = glm::sqrt(glm::max(
                    glm::make_vec3(p.material->pbr_metallic_roughness.base_color_factor), vec3(.02f)));
            const std::string_view name = node->name ? node->name : "";
            if (name.starts_with("COL_") || name.starts_with("TRIGGER_") || name.starts_with("PROBE_")
                || name.starts_with("FOG_"))
                glDeleteLists(list, 1);
            else
                model.parts.push_back({ list, int(node - data->nodes), tint, lo, hi });
        }
    for (size_t i = 0; i < node->children_count; ++i)
        meshNode(data, node->children[i], model);
}
}

API int vp_init(HWND hwnd)
{
    window = hwnd;
    dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pf { };
    pf.nSize = sizeof(pf);
    pf.nVersion = 1;
    pf.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pf.iPixelType = PFD_TYPE_RGBA;
    pf.cColorBits = 24;
    pf.cDepthBits = 24;
    int format = ChoosePixelFormat(dc, &pf);
    if (!format || !SetPixelFormat(dc, format, &pf) || !(context = wglCreateContext(dc))
        || !wglMakeCurrent(dc, context))
        return 0;
    buildBox();
    return 1;
}
API void vp_close()
{
    if (context) {
        glDeleteLists(box, 1);
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(context);
        context = nullptr;
    }
    if (dc) {
        ReleaseDC(window, dc);
        dc = nullptr;
    }
}
API Model* vp_load(const char* json, size_t length)
{
    cgltf_options options { };
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, json, length, &data) != cgltf_result_success)
        return nullptr;
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> holder(data, cgltf_free);
    if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success
        || cgltf_validate(data) != cgltf_result_success)
        return nullptr;
    auto model = std::make_unique<Model>();
    auto scene = data->scene ? data->scene : (data->scenes_count ? data->scenes : nullptr);
    if (!scene)
        return nullptr;
    for (size_t i = 0; i < scene->nodes_count; i++)
        meshNode(data, scene->nodes[i], *model);
    return model.release();
}
API void vp_free(Model* model) { delete model; }
API int vp_bounds(Model* model, float* out)
{
    if (!model || model->parts.empty())
        return 0;
    vec3 lo(1e30f), hi(-1e30f);
    for (auto& p : model->parts) {
        lo = glm::min(lo, p.lo);
        hi = glm::max(hi, p.hi);
    }
    std::copy_n(glm::value_ptr(lo), 3, out);
    std::copy_n(glm::value_ptr(hi), 3, out + 3);
    return 1;
}
API void vp_begin(int width, int height, const float* eye, const float* target, int pick)
{
    picking = pick;
    hits.clear();
    viewport = { 0, 0, width, height };
    auto center = glm::make_vec3(target);
    auto camera = glm::make_vec3(eye);
    float distance = glm::distance(center, camera);
    view = glm::lookAt(camera, center, vec3(0, 1, 0));
    projection = glm::perspective(glm::radians(48.f), float(width) / height,
        std::max(.015f, distance * .0005f), std::max(2000.f, distance * 8));
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(glm::value_ptr(projection));
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(glm::value_ptr(view));
    glClearColor(pick ? 0 : .073f, pick ? 0 : .09f, pick ? 0 : .105f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);
    glDisable(GL_LIGHTING);
    if (!pick) {
        float spacing = std::pow(10.f, std::floor(std::log10(distance / 8)));
        vec3 origin = glm::floor(center / spacing) * spacing;
        glBegin(GL_LINES);
        for (int i = -50; i <= 50; ++i) {
            paint(i % 5 ? vec3(.14f, .17f, .19f) : vec3(.22f, .26f, .28f));
            float x = origin.x + i * spacing, z = origin.z + i * spacing;
            vertex({ x, -.004f, origin.z - 50 * spacing });
            vertex({ x, -.004f, origin.z + 50 * spacing });
            vertex({ origin.x - 50 * spacing, -.004f, z });
            vertex({ origin.x + 50 * spacing, -.004f, z });
        }
        glEnd();
        // The work grid is a backdrop. Keep its depth out of the scene so
        // large floor surfaces do not fight coplanar grid lines.
        glClear(GL_DEPTH_BUFFER_BIT);
        GLfloat light[] = { .4f, 1, .7f, 0 }, ambient[] = { .35f, .35f, .35f, 1 };
        GLfloat diffuse[] = { .68f,.68f,.68f,1 };
        glLightfv(GL_LIGHT0,GL_DIFFUSE,diffuse);
        glLightfv(GL_LIGHT0, GL_POSITION, light);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
        glEnable(GL_NORMALIZE);
        glEnable(GL_LIGHT0);
        glEnable(GL_COLOR_MATERIAL);
    }
}
API void vp_object(
    int id, int kind, Model* model, const float* pos, const float* quat, const float* scale, int selected)
{
    glPushMatrix();
    glTranslatef(pos[0], pos[1], pos[2]);
    auto rot = glm::mat4_cast(glm::quat(quat[3], quat[0], quat[1], quat[2]));
    glMultMatrixf(glm::value_ptr(rot));
    glScalef(scale[0], scale[1], scale[2]);
    if (!picking)
        glEnable(GL_LIGHTING);
    auto render = [&](GLuint list, int node, vec3 tint) {
        color({ id, node, -1, -1 }, selected ? glm::mix(tint, vec3(1, .68f, .24f), .36f) : tint);
        glCallList(list);
    };
    if (model)
        for (auto& p : model->parts)
            render(p.list, p.node, p.color);
    else {
        if (kind == 2) {
            glTranslatef(0, .35f, 0);
            glScalef(.38f, .7f, .38f);
        }
        if (kind == 1 && !picking)
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        render(box, -1,
            kind == 1       ? vec3(1, .58f, .16f)
                : kind == 2 ? vec3(.32f, .8f, .52f)
                            : vec3(.49f, .61f, .68f));
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    glDisable(GL_LIGHTING);
    glPopMatrix();
}
API void vp_pin(int id, int note, const float* p, int selected)
{
    color({ id, -1, -1, note }, selected ? vec3(1, 1, .6f) : vec3(1, .72f, .16f));
    glDisable(GL_DEPTH_TEST);
    glPointSize(9);
    glBegin(GL_POINTS);
    vertex(glm::make_vec3(p));
    glEnd();
    glEnable(GL_DEPTH_TEST);
}
API void vp_gizmo(int id, int tool, const float* p, const float* q, int active)
{
    glPushMatrix();
    auto pos = glm::make_vec3(p);
    glTranslatef(p[0], p[1], p[2]);
    if (tool == 2)
        glMultMatrixf(glm::value_ptr(glm::mat4_cast(glm::quat(q[3], q[0], q[1], q[2]))));
    float length = std::max(.001f, -(view * glm::vec4(pos, 1)).z) * 64.f / viewport.w;
    glScalef(length, length, length);
    glDisable(GL_DEPTH_TEST);
    for (int i = 0; i < 3; i++) {
        vec3 axis(0);
        axis[i] = 1;
        color({ id, -1, i, -1 },
            active == i  ? vec3(1, .88f, .43f)
                : i == 0 ? vec3(.98f, .32f, .32f)
                : i == 1 ? vec3(.42f, .85f, .35f)
                         : vec3(.29f, .59f, 1));
        glLineWidth(picking ? 8 : 3);
        if (tool == 1) {
            glBegin(GL_LINE_LOOP);
            for (int j = 0; j < 72; j++) {
                vec3 v(0);
                float a = j * 6.2831853f / 72;
                v[(i + 1) % 3] = std::cos(a);
                v[(i + 2) % 3] = std::sin(a);
                vertex(v);
            }
            glEnd();
        } else {
            glBegin(GL_LINES);
            vertex(axis * .15f);
            vertex(axis);
            glEnd();
            if (tool == 2) {
                glPushMatrix();
                glTranslatef(axis.x, axis.y, axis.z);
                glScalef(.15f, .15f, .15f);
                glCallList(box);
                glPopMatrix();
            } else {
                glBegin(GL_TRIANGLE_FAN);
                vertex(axis * 1.19f);
                for (int j = 0; j <= 16; j++) {
                    auto v = axis * .93f;
                    float a = j * 6.2831853f / 16;
                    v[(i + 1) % 3] = .09f * std::cos(a);
                    v[(i + 2) % 3] = .09f * std::sin(a);
                    vertex(v);
                }
                glEnd();
            }
        }
    }
    glLineWidth(1);
    glEnable(GL_DEPTH_TEST);
    glPopMatrix();
}
API void vp_end() { SwapBuffers(dc); }
API int vp_hit(int x, int y, int* hit, float* point)
{
    unsigned char rgb[4] { };
    float z;
    glReadPixels(x, viewport.w - 1 - y, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glReadPixels(x, viewport.w - 1 - y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &z);
    int n = rgb[0] + 256 * rgb[1] + 65536 * rgb[2];
    if (!n || size_t(n) > hits.size())
        return 0;
    auto h = hits[n - 1];
    hit[0] = h.object;
    hit[1] = h.node;
    hit[2] = h.handle;
    hit[3] = h.note;
    auto pos = glm::unProject(vec3(x + .5f, viewport.w - y - .5f, z), view, projection, viewport);
    std::copy_n(glm::value_ptr(pos), 3, point);
    return 1;
}
