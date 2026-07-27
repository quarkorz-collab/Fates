#pragma once

#include <functional>

#if defined(FATES_USE_STD_CONTAINERS)
#include <unordered_map>
#include <unordered_set>
#else
#include "../third_party/unordered_dense/include/ankerl/unordered_dense.h"
#endif

namespace fates {

#if defined(FATES_USE_STD_CONTAINERS)

template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
using FastMap = std::unordered_map<Key, Value, Hash, Equal>;

template <typename Key,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
using FastSet = std::unordered_set<Key, Hash, Equal>;

inline constexpr const char* kContainerBackend = "std";

#else

template <typename Key,
          typename Value,
          typename Hash = ankerl::unordered_dense::hash<Key>,
          typename Equal = std::equal_to<Key>>
using FastMap = ankerl::unordered_dense::map<Key, Value, Hash, Equal>;

template <typename Key,
          typename Hash = ankerl::unordered_dense::hash<Key>,
          typename Equal = std::equal_to<Key>>
using FastSet = ankerl::unordered_dense::set<Key, Hash, Equal>;

inline constexpr const char* kContainerBackend = "unordered_dense";

#endif

}  // namespace fates
