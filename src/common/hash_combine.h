// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <functional>
#include <type_traits>

namespace HashCombineDetail
{
inline std::size_t Mix(std::size_t seed, std::size_t value)
{
  if constexpr (sizeof(std::size_t) == 8)
  {
    constexpr std::size_t OFFSET = static_cast<std::size_t>(0xCBF29CE484222325ull);
    constexpr std::size_t PRIME = static_cast<std::size_t>(0x100000001B3ull);
    seed ^= value + OFFSET;
    seed *= PRIME;
    seed ^= (seed >> 32);
  }
  else
  {
    constexpr std::size_t OFFSET = static_cast<std::size_t>(0x811C9DC5u);
    constexpr std::size_t PRIME = static_cast<std::size_t>(0x01000193u);
    seed ^= value + OFFSET;
    seed *= PRIME;
    seed ^= (seed >> 16);
  }

  return seed;
}
} // namespace HashCombineDetail

template<typename T, typename... Rest>
void hash_combine(std::size_t& seed, const T& value, const Rest&... rest)
{
  const auto append = [&seed](const auto& item) {
    using ValueType = std::decay_t<decltype(item)>;
    seed = HashCombineDetail::Mix(seed, std::hash<ValueType>{}(item));
  };

  append(value);
  (append(rest), ...);
}
