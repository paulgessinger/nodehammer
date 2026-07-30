#include "project_panel.hpp"

#include <viewer/project_fs.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nodehammer::viewer::ui {
namespace {

/// ImGui drag-drop payload type for a Project-tree file (payload = full key).
constexpr const char *kFileDragType = "NH_PROJECT_FILE";

std::string normalizedSelectionKey(std::string_view key) {
    auto out = std::filesystem::path{key}.lexically_normal().generic_string();
    if (!out.empty() && out.front() == '/') {
        out.erase(out.begin());
    }
    return out;
}

std::string lowerExtension(const std::filesystem::path &path) {
    auto ext = path.extension().string();
    for (auto &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

bool isFlatBufferGeometry(std::string_view key) {
    auto path = std::filesystem::path{key};
    const auto ext = lowerExtension(path);
    if (ext == ".nhb") {
        return true;
    }
    if (ext != ".zst") {
        return false;
    }
    return lowerExtension(path.stem()) == ".nhb";
}

const char *statusLabel(ProjectFsStatus status) {
    switch (status) {
    case ProjectFsStatus::Idle:
        return "idle";
    case ProjectFsStatus::Fetching:
        return "fetching";
    case ProjectFsStatus::Ready:
        return "ready";
    case ProjectFsStatus::Error:
        return "error";
    }
    return "?";
}

} // namespace

void renderProjectPanel(bool *open, const ViewerUiContext &ctx, const UiActions &actions) {
    if (!ImGui::Begin("Project", open)) {
        ImGui::End();
        return;
    }

    if (ctx.project == nullptr) {
        ImGui::TextDisabled("(no project)");
        ImGui::End();
        return;
    }

    const auto backend_name = ctx.project->name();
    ImGui::Text("backend: %.*s   status: %s", static_cast<int>(backend_name.size()),
                backend_name.data(), statusLabel(ctx.project->status()));

    const auto root_nodes = ctx.project->list("");
    if (root_nodes.empty()) {
        ImGui::TextDisabled("(no files yet)");
        ImGui::End();
        return;
    }

    const auto selected_config_key = normalizedSelectionKey(ctx.root_config_key);
    const auto selected_geometry_key = normalizedSelectionKey(ctx.root_geometry_key);

    std::unordered_map<std::string_view, const ProjectProgress *> progress_by_key;
    for (const auto &progress : ctx.project->progress()) {
        progress_by_key.emplace(progress.url, &progress);
    }

    auto renderNodes = [&](this const auto &self, std::span<const DirNode> nodes) -> void {
        for (const auto &node : nodes) {
            ImGui::PushID(node.key.c_str());
            if (node.is_directory) {
                // Drive the open/closed state from the persisted set so it
                // survives restarts (ImGui's .ini never stores tree state).
                // ImGuiCond_Always makes the set the single source of truth;
                // user toggles are written back below.
                const bool want_open =
                    ctx.project_tree_open != nullptr && ctx.project_tree_open->contains(node.key);
                ImGui::SetNextItemOpen(want_open, ImGuiCond_Always);
                const bool open_node =
                    ImGui::TreeNodeEx("##dir", ImGuiTreeNodeFlags_None, "%s", node.name.c_str());
                if (ImGui::IsItemToggledOpen() && ctx.project_tree_open != nullptr) {
                    if (open_node) {
                        ctx.project_tree_open->insert(node.key);
                    } else {
                        ctx.project_tree_open->erase(node.key);
                    }
                    ImGui::MarkIniSettingsDirty();
                }
                // Drop a dragged file here to move it into this folder. Must be
                // registered against the directory's tree node before any child
                // items are submitted below.
                if (actions.move_key && ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload(kFileDragType)) {
                        actions.move_key(std::string{static_cast<const char *>(pl->Data)},
                                         node.key);
                    }
                    ImGui::EndDragDropTarget();
                }
                if (open_node) {
                    self(ctx.project->list(node.key));
                    ImGui::TreePop();
                }
                ImGui::PopID();
                continue;
            }

            if (auto it = progress_by_key.find(node.key); it != progress_by_key.end()) {
                const auto &progress = *it->second;
                if (progress.failed) {
                    ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "[fail]");
                    ImGui::SameLine();
                } else if (!progress.done && progress.bytes_total > 0) {
                    const float frac =
                        static_cast<float>(static_cast<double>(progress.bytes_done) /
                                           static_cast<double>(progress.bytes_total));
                    ImGui::ProgressBar(frac, ImVec2(60.f, 0.f));
                    ImGui::SameLine();
                } else if (!progress.done) {
                    ImGui::ProgressBar(-1.f * static_cast<float>(ImGui::GetTime()),
                                       ImVec2(60.f, 0.f), "");
                    ImGui::SameLine();
                }
            }

            const auto tree_key = normalizedSelectionKey(node.key);
            const bool is_config = !selected_config_key.empty() && tree_key == selected_config_key;
            const bool is_geometry =
                !selected_geometry_key.empty() && tree_key == selected_geometry_key;

            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (is_config || is_geometry) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }

            const char *tag = is_config ? " [config]" : is_geometry ? " [geometry]" : "";
            ImGui::TreeNodeEx("##leaf", flags, "%s%s", node.name.c_str(), tag);

            // Drag a file onto a directory node to move it into that folder. The
            // payload is the file's full key (null-terminated).
            if (actions.move_key && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload(kFileDragType, node.key.c_str(), node.key.size() + 1);
                ImGui::Text("Move %s", node.name.c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (lowerExtension(std::filesystem::path{node.key}) == ".toml") {
                    if (actions.select_config_key) {
                        actions.select_config_key(node.key);
                    }
                } else if (isFlatBufferGeometry(node.key)) {
                    if (actions.select_geometry_key) {
                        actions.select_geometry_key(node.key);
                    }
                }
            }

            // Right-click a file for its actions. Removal routes through the
            // App, which asks the backend (planRemove) and shows a confirmation
            // modal before committing — read-only backends will surface a reject.
            if (actions.remove_key && ImGui::BeginPopupContextItem("##file_ctx")) {
                if (ImGui::MenuItem("Remove…")) {
                    actions.remove_key(node.key);
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
    };
    renderNodes(root_nodes);

    if (ImGui::Button("Rescan") && actions.rescan_project) {
        actions.rescan_project();
    }

    ImGui::End();
}

} // namespace nodehammer::viewer::ui
