#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace horror {

// Very light handle-based cache keyed by a logical path string.
//
// Everything is shared_ptr<T> so lifetime is owned by whoever still references
// it; the cache keeps weak_ptrs so a resource evicted from the cache (or
// explicitly released via `reset`) vanishes properly. Handles handle their own
// GPU cleanup in RAII. No background loading for now (font -> boring feature
// creep, add an asset thread later).
template<typename T, typename... LoadArgs>
class ResourceCache {
public:
    using ResourcePtr = std::shared_ptr<T>;

    ResourcePtr get(const std::string &key, LoadArgs &&...args)
    {
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            if (auto locked = it->second.lock())
                return locked;
        }
        ResourcePtr res = std::make_shared<T>(std::forward<LoadArgs>(args)...);
        entries_[key] = res;
        return res;
    }

    void clear() { entries_.clear(); }
    size_t liveCount() const
    {
        size_t count = 0;
        for (const auto &e : entries_)
            if (e.second.lock())
                ++count;
        return count;
    }

private:
    std::unordered_map<std::string, std::weak_ptr<T>> entries_;
};

} // namespace horror
