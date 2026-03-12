#pragma once

#include "object.h"

#include <memory>
#include <utility>
#include <vector>

// Single allocation hook — future GC bookkeeping lives here.
template <typename T, typename... Args>
T* allocateObj(std::vector<std::unique_ptr<Obj>>& objects, Args&&... args) {
    auto obj = std::make_unique<T>(std::forward<Args>(args)...);
    T* ptr = obj.get();
    objects.emplace_back(std::move(obj));
    return ptr;
}
