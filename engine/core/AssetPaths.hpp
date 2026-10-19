#pragma once

#include <filesystem>
#include <string>

namespace horror {

// Resolves a logical asset path ("test/Fox.glb") against, in order:
//   1. the path itself when absolute,
//   2. "assets/<path>" relative to the current directory,
//   3. "../assets/<path>" (running the binary from build/),
//   4. the source-tree assets directory baked in at configure time.
// Loading code gets a usable path regardless of where the game was started;
// missing files fall through to the last candidate so the caller can report a
// meaningful error.
std::filesystem::path resolveAssetPath(const std::string &logicalPath);

// Directory part of a resolved model path, for resolving external image URIs.
std::filesystem::path assetDirectory(const std::string &resolvedModelPath);

} // namespace horror
