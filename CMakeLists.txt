cmake_minimum_required(VERSION 3.24)
project(HorrorEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Silence min_required complaints from older dep subprojects under CMake >= 3.27.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Build dependency sub-projects the way we want, not theirs.
set(BUILD_SHARED_LIBS OFF)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_DOCS OFF)
set(GLFW_INSTALL OFF)

include(FetchContent)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9b
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glfw glm imgui)

# Dear ImGui ships no CMake project; assemble a static lib from its sources.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

# GLAD loader generated with `glad --api="gl:core=4.5"`; checked in so builds
# don't need python/network. Regenerate only when bumping the GL version.
add_library(glad STATIC external/glad/src/gl.c)
target_include_directories(glad PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/include)

# --- Engine library ----------------------------------------------------------
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS engine/*.cpp)
add_library(engine STATIC ${ENGINE_SOURCES})
target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/external/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/external/cgltf
)
# Absolute fallbacks so shaders/assets resolve when the game is started from
# an IDE or any other working directory. Runtime-relative lookup still wins.
target_compile_definitions(engine PRIVATE
    HOR_SHADER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    HOR_ASSET_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
)
target_link_libraries(engine PUBLIC glad glm::glm glfw imgui)

# --- Game executable ----------------------------------------------------------
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS game/*.cpp)
add_executable(game ${GAME_SOURCES})
target_link_libraries(game PRIVATE engine)

# Shader sync as its own ALL target: a POST_BUILD hook on `game` would skip
# when the binary is unchanged, and stale shaders then silently defeat the
# in-engine hot reload (mtime unchanged).
add_custom_target(copy_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:game>/shaders
    COMMENT "Syncing shaders next to the executable")
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:game>/assets
    COMMENT "Syncing assets next to the executable")
add_dependencies(game copy_shaders copy_assets)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_SOURCES} ${GAME_SOURCES})

install(TARGETS engine game)
