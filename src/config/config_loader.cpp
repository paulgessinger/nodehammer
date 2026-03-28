#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

#include <toml++/toml.hpp>

#include <format>

namespace nodehammer {

namespace {

// ── Enum parsers ──────────────────────────────────────────────────────────────

std::optional<ClosurePolicy> parseClosure(std::string_view s) {
    if (s == "none") {
        return ClosurePolicy::None;
    }
    if (s == "ancestors") {
        return ClosurePolicy::Ancestors;
    }
    if (s == "descendants") {
        return ClosurePolicy::Descendants;
    }
    if (s == "full") {
        return ClosurePolicy::Full;
    }
    return std::nullopt;
}

std::optional<SelectionAction> parseAction(std::string_view s) {
    if (s == "keep_if") {
        return SelectionAction::KeepIf;
    }
    if (s == "drop_if") {
        return SelectionAction::DropIf;
    }
    return std::nullopt;
}

std::optional<BooleanFallback> parseFallback(std::string_view s) {
    if (s == "skip") {
        return BooleanFallback::Skip;
    }
    if (s == "bbox") {
        return BooleanFallback::BBox;
    }
    if (s == "fail") {
        return BooleanFallback::Fail;
    }
    return std::nullopt;
}

// ── Predicate parser (recursive) ─────────────────────────────────────────────

std::optional<PredicateExpr> parsePredicate(const toml::table &tbl, DiagnosticList &diags,
                                            std::string_view context);

std::optional<PredicateExpr> parsePredicate(const toml::table &tbl, DiagnosticList &diags,
                                            std::string_view context) {
    auto typeOpt = tbl["type"].value<std::string>();
    if (!typeOpt) {
        diags.error(std::string{codes::kErrConfigParse},
                    "predicate is missing required 'type' field", std::string{context});
        return std::nullopt;
    }
    const std::string &type = *typeOpt;

    if (type == "name_glob") {
        auto pat = tbl["pattern"].value<std::string>();
        if (!pat) {
            diags.error(std::string{codes::kErrConfigParse},
                        "name_glob predicate missing 'pattern'", std::string{context});
            return std::nullopt;
        }
        return PredicateExpr{NameGlobPredicate{*pat}};
    }

    if (type == "path_glob") {
        auto pat = tbl["pattern"].value<std::string>();
        if (!pat) {
            diags.error(std::string{codes::kErrConfigParse},
                        "path_glob predicate missing 'pattern'", std::string{context});
            return std::nullopt;
        }
        return PredicateExpr{PathGlobPredicate{*pat}};
    }

    if (type == "tag") {
        auto key = tbl["key"].value<std::string>();
        if (!key) {
            diags.error(std::string{codes::kErrConfigParse}, "tag predicate missing 'key'",
                        std::string{context});
            return std::nullopt;
        }
        auto value = tbl["value"].value<std::string>();
        return PredicateExpr{TagPredicate{*key, value}};
    }

    if (type == "is_leaf") {
        return PredicateExpr{IsLeafPredicate{}};
    }

    if (type == "and" || type == "or") {
        auto *operandsArr = tbl["operands"].as_array();
        if (!operandsArr) {
            diags.error(std::string{codes::kErrConfigParse},
                        std::format("'{}' predicate missing 'operands' array", type),
                        std::string{context});
            return std::nullopt;
        }
        std::vector<PredicateExpr> operands;
        operands.reserve(operandsArr->size());
        for (const auto &elem : *operandsArr) {
            const auto *subTbl = elem.as_table();
            if (!subTbl) {
                diags.error(std::string{codes::kErrConfigParse},
                            "operand in compound predicate must be a table", std::string{context});
                return std::nullopt;
            }
            auto sub = parsePredicate(*subTbl, diags, context);
            if (!sub) {
                return std::nullopt;
            }
            operands.push_back(std::move(*sub));
        }
        if (type == "and") {
            return PredicateExpr{std::make_shared<AndPredicate>(std::move(operands))};
        }
        return PredicateExpr{std::make_shared<OrPredicate>(std::move(operands))};
    }

    if (type == "not") {
        const auto *operandTbl = tbl["operand"].as_table();
        if (!operandTbl) {
            diags.error(std::string{codes::kErrConfigParse},
                        "'not' predicate missing 'operand' table", std::string{context});
            return std::nullopt;
        }
        auto sub = parsePredicate(*operandTbl, diags, context);
        if (!sub) {
            return std::nullopt;
        }
        return PredicateExpr{std::make_shared<NotPredicate>(std::move(*sub))};
    }

    diags.error(std::string{codes::kErrConfigParse},
                std::format("unknown predicate type '{}'", type), std::string{context});
    return std::nullopt;
}

// ── Section parsers ───────────────────────────────────────────────────────────

glm::vec4 parseVec4(const toml::array &arr, glm::vec4 fallback) {
    if (arr.size() < 4) {
        return fallback;
    }
    return glm::vec4{
        arr.at(0).value<float>().value_or(fallback.r),
        arr.at(1).value<float>().value_or(fallback.g),
        arr.at(2).value<float>().value_or(fallback.b),
        arr.at(3).value<float>().value_or(fallback.a),
    };
}

glm::vec3 parseVec3(const toml::array &arr, glm::vec3 fallback) {
    if (arr.size() < 3) {
        return fallback;
    }
    return glm::vec3{
        arr.at(0).value<float>().value_or(fallback.r),
        arr.at(1).value<float>().value_or(fallback.g),
        arr.at(2).value<float>().value_or(fallback.b),
    };
}

void parseMaterials(const toml::table &root, NHConfig &cfg, DiagnosticList &diags) {
    const auto *arr = root["materials"].as_array();
    if (!arr) {
        return;
    }
    for (const auto &entry : *arr) {
        const auto *tbl = entry.as_table();
        if (!tbl) {
            continue;
        }
        auto name = (*tbl)["name"].value<std::string>();
        if (!name) {
            diags.error(std::string{codes::kErrConfigParse},
                        "material entry missing required 'name' field");
            continue;
        }
        MaterialDef def;
        def.name = *name;

        if (const auto *colorArr = (*tbl)["base_color"].as_array()) {
            def.baseColor = parseVec4(*colorArr, def.baseColor);
        }
        def.metallic = (*tbl)["metallic"].value<float>().value_or(def.metallic);
        def.roughness = (*tbl)["roughness"].value<float>().value_or(def.roughness);
        def.doubleSided = (*tbl)["double_sided"].value<bool>().value_or(def.doubleSided);
        if (const auto *emissArr = (*tbl)["emissive"].as_array()) {
            def.emissive = parseVec3(*emissArr, def.emissive);
        }
        cfg.materials.push_back(std::move(def));
    }
}

void parseSelection(const toml::table &root, NHConfig &cfg, DiagnosticList &diags) {
    const auto *arr = root["selection"].as_array();
    if (!arr) {
        return;
    }
    for (const auto &entry : *arr) {
        const auto *tbl = entry.as_table();
        if (!tbl) {
            continue;
        }
        auto actionStr = (*tbl)["action"].value<std::string>();
        if (!actionStr) {
            diags.error(std::string{codes::kErrConfigParse},
                        "selection rule missing required 'action' field");
            continue;
        }
        auto action = parseAction(*actionStr);
        if (!action) {
            diags.error(std::string{codes::kErrConfigParse},
                        std::format("unknown selection action '{}'; expected keep_if or drop_if",
                                    *actionStr));
            continue;
        }

        const auto *predTbl = (*tbl)["predicate"].as_table();
        if (!predTbl) {
            diags.error(std::string{codes::kErrConfigParse},
                        "selection rule missing required 'predicate' table");
            continue;
        }
        auto pred = parsePredicate(*predTbl, diags, "selection");
        if (!pred) {
            continue;
        }

        ClosurePolicy closure = ClosurePolicy::None;
        if (auto closureStr = (*tbl)["closure"].value<std::string>()) {
            auto parsed = parseClosure(*closureStr);
            if (!parsed) {
                diags.warn(std::string{codes::kErrConfigParse},
                           std::format("unknown closure '{}'; defaulting to 'none'", *closureStr));
            } else {
                closure = *parsed;
            }
        }

        cfg.selection.push_back(SelectionRule{*action, std::move(*pred), closure});
    }
}

void parseMaterialRules(const toml::table &root, NHConfig &cfg, DiagnosticList &diags) {
    const auto *arr = root["material_rules"].as_array();
    if (!arr) {
        return;
    }
    for (const auto &entry : *arr) {
        const auto *tbl = entry.as_table();
        if (!tbl) {
            continue;
        }
        auto nameGlob = (*tbl)["name_glob"].value<std::string>();
        auto material = (*tbl)["material"].value<std::string>();
        if (!material) {
            diags.error(std::string{codes::kErrConfigParse},
                        "material_rule missing required 'material' field");
            continue;
        }
        MaterialRule rule;
        rule.nameGlob = nameGlob.value_or("*");
        rule.materialName = *material;
        cfg.materialRules.push_back(std::move(rule));
    }
}

void parseTessellationRules(const toml::table &root, NHConfig &cfg, DiagnosticList &diags) {
    const auto *arr = root["tessellation_rules"].as_array();
    if (!arr) {
        return;
    }
    for (const auto &entry : *arr) {
        const auto *tbl = entry.as_table();
        if (!tbl) {
            continue;
        }
        TessellationRule rule;
        rule.nameGlob = (*tbl)["name_glob"].value<std::string>().value_or("*");
        rule.maxSegmentsCircle = (*tbl)["max_segments_circle"].value<int>().value_or(64);

        if (auto fallbackStr = (*tbl)["fallback"].value<std::string>()) {
            auto parsed = parseFallback(*fallbackStr);
            if (!parsed) {
                diags.warn(
                    std::string{codes::kErrConfigParse},
                    std::format("unknown fallback '{}'; defaulting to 'skip'", *fallbackStr));
            } else {
                rule.fallback = *parsed;
            }
        }
        cfg.tessellationRules.push_back(std::move(rule));
    }
}

void parseOutput(const toml::table &root, NHConfig &cfg, DiagnosticList & /*diags*/) {
    const auto *tbl = root["output"].as_table();
    if (!tbl) {
        return;
    }
    if (auto path = (*tbl)["path"].value<std::string>()) {
        cfg.exportCfg.outputPath = *path;
    }
    cfg.exportCfg.format = (*tbl)["format"].value<std::string>().value_or("");
    cfg.exportCfg.embedExtras = (*tbl)["embed_extras"].value<bool>().value_or(false);
}

// ── Top-level parse driver ────────────────────────────────────────────────────

std::expected<NHConfig, DiagnosticList> parseTable(const toml::table &tbl) {
    DiagnosticList diags;
    NHConfig cfg;

    parseOutput(tbl, cfg, diags);
    parseMaterials(tbl, cfg, diags);
    parseSelection(tbl, cfg, diags);
    parseMaterialRules(tbl, cfg, diags);
    parseTessellationRules(tbl, cfg, diags);

    if (diags.hasErrors()) {
        return std::unexpected(std::move(diags));
    }
    return cfg;
}

} // namespace

// ── ConfigLoader ──────────────────────────────────────────────────────────────

std::expected<NHConfig, DiagnosticList>
ConfigLoader::loadFromFile(const std::filesystem::path &path) {
    DiagnosticList diags;
    try {
        auto tbl = toml::parse_file(path.string());
        return parseTable(tbl);
    } catch (const toml::parse_error &e) {
        diags.error(
            std::string{codes::kErrConfigParse}, std::format("{}", e.description()),
            std::format("{}:{}:{}", path.string(), e.source().begin.line, e.source().begin.column));
        return std::unexpected(std::move(diags));
    } catch (const std::exception &e) {
        diags.error(std::string{codes::kErrImportFileNotFound},
                    std::format("could not read config file: {}", e.what()), path.string());
        return std::unexpected(std::move(diags));
    }
}

std::expected<NHConfig, DiagnosticList> ConfigLoader::loadFromString(std::string_view content,
                                                                     std::string_view sourceName) {
    DiagnosticList diags;
    try {
        auto tbl = toml::parse(content, sourceName);
        return parseTable(tbl);
    } catch (const toml::parse_error &e) {
        diags.error(
            std::string{codes::kErrConfigParse}, std::format("{}", e.description()),
            std::format("{}:{}:{}", sourceName, e.source().begin.line, e.source().begin.column));
        return std::unexpected(std::move(diags));
    }
}

} // namespace nodehammer
