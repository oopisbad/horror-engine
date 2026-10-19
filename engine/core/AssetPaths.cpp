#include "core/AssetPaths.hpp"

namespace horror {

#ifndef HOR_ASSET_SRC_DIR
#define HOR_ASSET_SRC_DIR "../assets"
#endif

std::filesystem::path resolveAssetPath(const std::string &logicalPath)
{
    namespace fs = std::filesystem;
    const fs::path logical(logicalPath);
    if (logical.is_absolute() && fs::exists(logical))
        return logical;

    const fs::path candidates[] = {
        fs::path("assets") / logical,
        fs::path("..") / "assets" / logical,
        fs::path(HOR_ASSET_SRC_DIR) / logical,
        logical,
    };

    std::error_code ec;
    for (const fs::path &candidate : candidates) {
        if (fs::exists(candidate, ec))
            return fs::absolute(candidate, ec);
    }
    return logical;
}

std::filesystem::path assetDirectory(const std::string &resolvedModelPath)
{
    return std::filesystem::path(resolvedModelPath).parent_path();
}

} // namespace horror
