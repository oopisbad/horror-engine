#pragma once

#include "scene/Components.hpp"

namespace horror {
class Application;
struct LevelInstance;
bool checkImportLab(Application &app, LevelInstance &level, EntityId player);
}
