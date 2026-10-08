#pragma once
#include <cstddef>
#include <ir/semantic.hpp>
#include <span>
#include <vector>

namespace nodehammer::ir {
// Normal worker/processing transport uses the canonical scene codec.
std::vector<std::byte> semanticSceneToBytes(const semantic::Scene &scene);
semantic::Scene semanticSceneFromBytes(std::span<const std::byte> bytes);
} // namespace nodehammer::ir
