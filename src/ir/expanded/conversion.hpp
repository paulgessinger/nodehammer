#pragma once
#include <functional>
#include <ir/expanded/scene.hpp>
#include <ir/semantic/occurrence_tree.hpp>
namespace nodehammer::config {
struct SelectionRule;
}
namespace nodehammer::ir::semantic {
expanded::Scene expand(const Scene &scene);
DiagnosticList select(Scene &scene, const std::vector<config::SelectionRule> &rules, bool hoist);
void visit(const Scene &scene, const std::function<bool(const Occurrence &, bool)> &fn);
} // namespace nodehammer::ir::semantic
