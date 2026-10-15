#pragma once
#include <functional>
#include <ir/expanded/scene.hpp>
#include <ir/semantic/occurrence_tree.hpp>
namespace nodehammer::ir::semantic {
expanded::Scene expand(const Scene &scene);
void visit(const Scene &scene, const std::function<bool(const Occurrence &, bool)> &fn);
} // namespace nodehammer::ir::semantic
