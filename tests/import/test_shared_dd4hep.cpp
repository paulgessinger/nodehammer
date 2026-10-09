#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <diagnostic_codes.hpp>
#include <ir/dd4hep/semantic/shared_importer.hpp>
#include <ir/tgeo/semantic/importer.hpp>
#include <ir/tgeo/semantic/shared_importer.hpp>
#include <selection/occurrence_selector.hpp>

#include <DD4hep/DetElement.h>
#include <DD4hep/Detector.h>
#include <DD4hep/Shapes.h>
#include <DD4hep/Volumes.h>

#include <TGeoManager.h>
#include <TGeoMatrix.h>
#include <TGeoNode.h>
#include <TGeoVolume.h>

#include <memory>
#include <string>

namespace {
namespace semantic = nodehammer::ir::semantic;
using nodehammer::ir::semantic::OccurrenceId;

struct Fixture {
    std::unique_ptr<dd4hep::Detector> detector = dd4hep::Detector::make_unique("");
    dd4hep::Volume module;
    dd4hep::PlacedVolume left;
    dd4hep::PlacedVolume right;
    dd4hep::PlacedVolume sensor;
    std::size_t leftIndex{};
    std::size_t rightIndex{};

    Fixture() {
        detector->fromCompact(std::string{NODEHAMMER_FIXTURES_DIR} + "/dd4hep/shared_world.xml");
        module = dd4hep::Volume{"shared_module", dd4hep::Box{20, 20, 20}, detector->air()};
        dd4hep::Volume sensitive{"sensor", dd4hep::Box{1, 1, 1}, detector->air()};
        dd4hep::Volume passive{"passive", dd4hep::Box{1, 1, 1}, detector->air()};
        dd4hep::SensitiveDetector readout{"SharedSensorReadout", "tracker"};
        detector->addSensitiveDetector(readout);
        sensitive.setSensitiveDetector(readout);
        sensor = module.placeVolume(sensitive, dd4hep::Position{1, 2, 3});
        module.placeVolume(passive, dd4hep::Position{4, 5, 6});
        const auto world = detector->worldVolume();
        leftIndex = static_cast<std::size_t>(world->GetNdaughters());
        left = world.placeVolume(module, dd4hep::Position{-100, 0, 0});
        rightIndex = static_cast<std::size_t>(world->GetNdaughters());
        right = world.placeVolume(module, dd4hep::Position{100, 0, 0});
        detector->endDocument();
    }

    void addAnchoredElements() {
        dd4hep::DetElement leftElement{detector->world(), "LeftDisplay", 101};
        leftElement.setPlacement(left);
        leftElement.setType("left_module_type");
        dd4hep::DetElement rightElement{detector->world(), "RightDisplay", 102};
        rightElement.setPlacement(right);
        rightElement.setType("right_module_type");
        dd4hep::DetElement leftSensor{leftElement, "LeftSensorDisplay", 103};
        leftSensor.setPlacement(sensor);
        leftSensor.setType("left_sensor_type");
        dd4hep::DetElement rightSensor{rightElement, "RightSensorDisplay", 104};
        rightSensor.setPlacement(sensor);
        rightSensor.setType("right_sensor_type");
    }
};

bool hasCode(const nodehammer::DiagnosticList &diags, std::string_view code) {
    for (const auto &diagnostic : diags) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("shared DD4hep import preserves repeated sensitive occurrences and explicit metadata",
          "[import][dd4hep][shared]") {
    Fixture fixture;
    fixture.addAnchoredElements();
    // Name-based placement paths cannot distinguish these two anchors.
    fixture.left->SetName("duplicate_module_name");
    fixture.right->SetName("duplicate_module_name");
    auto *manager = &fixture.detector->manager();
    auto *world = fixture.detector->worldVolume().ptr();
    auto *globalBefore = gGeoManager;
    const auto originalDaughters = world->GetNdaughters();

    auto result = semantic::importDD4hep(*fixture.detector, "borrowed.xml");
    REQUIRE_FALSE(result.diags.hasErrors());

    REQUIRE(result.scene.sourceFile == "borrowed.xml");
    REQUIRE(result.scene.validate() == 7);
    REQUIRE(result.scene.metadata.volumeTags.size() == 1);
    REQUIRE(&fixture.detector->manager() == manager);
    REQUIRE(fixture.detector->worldVolume().ptr() == world);
    REQUIRE(world->GetNdaughters() == originalDaughters);
    REQUIRE(gGeoManager == globalBefore);

    auto tree = result.scene.makeTree(1);
    const OccurrenceId leftId{fixture.leftIndex, 0};
    const OccurrenceId rightId{fixture.rightIndex, 0};
    const auto left = tree->materialize(leftId);
    const auto right = tree->materialize(rightId);
    REQUIRE(left->logVolId == right->logVolId);
    REQUIRE(left->name == "LeftSensorDisplay");
    REQUIRE(right->name == "RightSensorDisplay");
    REQUIRE(left->tags.at("sensitive") == "true");
    REQUIRE(right->tags.at("sensitive") == "true");
    REQUIRE(left->tags.at("subdetector") == "left_sensor_type");
    REQUIRE(right->tags.at("subdetector") == "right_sensor_type");
    REQUIRE(left->sourceSystem == "dd4hep");
    REQUIRE(right->sourceSystem == "dd4hep");
    REQUIRE(left->originalPath == "/world/LeftDisplay/LeftSensorDisplay");
    REQUIRE(right->originalPath == "/world/RightDisplay/RightSensorDisplay");
    // Import-time names affect config paths; later display edits must not.
    tree->setDisplayName({fixture.leftIndex}, "Later rename");
    REQUIRE(tree->materialize(leftId)->originalPath == left->originalPath);
    nodehammer::selection::SelectedOccurrences selected{*tree};
    nodehammer::config::SelectionRule drop;
    drop.action = nodehammer::config::SelectionAction::DropIf;
    drop.predicate = nodehammer::config::PredicateExpr{
        nodehammer::config::PathGlobPredicate{"/world/LeftDisplay/LeftSensorDisplay"}};
    const auto filtered = selected.prune({drop});
    REQUIRE(filtered.size() == 6);
    REQUIRE_FALSE(filtered.contains(leftId));
    REQUIRE(filtered.contains(rightId));
    REQUIRE(left->worldTransform[3][0] == Catch::Approx(-99));
    REQUIRE(right->worldTransform[3][0] == Catch::Approx(101));

    // Type annotations on the module are explicit, not inherited by children.
    const auto passive = tree->materialize({fixture.leftIndex, 1});
    REQUIRE(passive->tags.empty());
    REQUIRE(passive->sourceSystem == "dd4hep/tgeo");
    tree->clearCache();
    REQUIRE(tree->materialize(leftId)->tags == left->tags);
    // The returned graph and pinned snapshots own their data, not ROOT pointers.
    fixture.detector.reset();
    REQUIRE(tree->materialize(rightId)->name == "RightSensorDisplay");
    REQUIRE(left->tags.at("sensitive") == "true");
}

TEST_CASE("shared DD4hep import reports ambiguous repeated placements without guessing",
          "[import][dd4hep][shared]") {
    Fixture fixture;
    dd4hep::DetElement ambiguous{fixture.detector->world(), "AmbiguousSensor", 201};
    // No module DetElement anchors: both module copies contain this exact pointer.
    ambiguous.setPlacement(fixture.sensor);
    auto result = semantic::importDD4hep(*fixture.detector);
    REQUIRE(result.diags.hasErrors());
    REQUIRE(hasCode(result.diags, nodehammer::codes::kErrImportPlacementUnresolved));
    REQUIRE_FALSE(result.scene.metadata.displayNames.contains({fixture.leftIndex, 0}));
    REQUIRE_FALSE(result.scene.metadata.displayNames.contains({fixture.rightIndex, 0}));
    auto tree = result.scene.makeTree();
    REQUIRE(tree->materialize({fixture.leftIndex, 0})->tags.at("sensitive") == "true");
    REQUIRE(tree->materialize({fixture.rightIndex, 0})->tags.at("sensitive") == "true");
}

TEST_CASE("shared DD4hep import continues below an unplaced detector element",
          "[import][dd4hep][shared]") {
    Fixture fixture;
    dd4hep::DetElement unplaced{fixture.detector->world(), "UnplacedGrouping", 301};
    dd4hep::DetElement module{unplaced, "PlacedBelowGrouping", 302};
    module.setPlacement(fixture.left);
    dd4hep::DetElement sensor{module, "SensorBelowGrouping", 303};
    sensor.setPlacement(fixture.sensor);
    auto result = semantic::importDD4hep(*fixture.detector);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(hasCode(result.diags, nodehammer::codes::kWarnImportUnplacedAncestor));
    auto tree = result.scene.makeTree();
    REQUIRE(tree->materialize({fixture.leftIndex})->name == "PlacedBelowGrouping");
    REQUIRE(tree->materialize({fixture.leftIndex, 0})->name == "SensorBelowGrouping");
    REQUIRE(tree->materialize({fixture.rightIndex, 0})->name == fixture.sensor->GetName());
}

TEST_CASE("shared DD4hep import detects conflicting annotations on one occurrence",
          "[import][dd4hep][shared]") {
    Fixture fixture;
    dd4hep::DetElement first{fixture.detector->world(), "FirstAnnotation", 401};
    first.setPlacement(fixture.left);
    dd4hep::DetElement second{fixture.detector->world(), "SecondAnnotation", 402};
    second.setPlacement(fixture.left);
    auto result = semantic::importDD4hep(*fixture.detector);
    REQUIRE(result.diags.hasErrors());
    REQUIRE(hasCode(result.diags, nodehammer::codes::kErrImportPlacementUnresolved));
}

TEST_CASE("shared TGeo import matches eager root-origin policy without expanded nodes",
          "[import][dd4hep][tgeo][shared]") {
    Fixture fixture;
    auto *root = dynamic_cast<TGeoNodeMatrix *>(fixture.detector->manager().GetTopNode());
    REQUIRE(root != nullptr);
    root->SetMatrix(new TGeoTranslation(17, 18, 19));
    auto source = semantic::importTGeo(&fixture.detector->manager());
    nodehammer::ir::TGeoImporter eager;
    auto expanded = eager.importExpanded(&fixture.detector->manager());

    REQUIRE(source.scene.validate() == expanded.scene.nodes.size());
    REQUIRE(source.scene.logVols.size() == expanded.scene.logVols.size());
    REQUIRE(source.scene.shapes.size() == expanded.scene.shapes.size());
    auto tree = source.scene.makeTree();
    const auto sharedRoot = tree->materialize({});
    const auto &eagerRoot = expanded.scene.nodes.at(expanded.scene.rootId);
    REQUIRE(sharedRoot->localTransform == eagerRoot.localTransform);
    REQUIRE(sharedRoot->worldTransform == eagerRoot.worldTransform);
    REQUIRE(sharedRoot->originalPath == eagerRoot.originalPath);
    REQUIRE(sharedRoot->sourceSystem == "tgeo");
    REQUIRE(tree->materialize({fixture.rightIndex, 0})->worldTransform[3][0] == Catch::Approx(101));
    REQUIRE_THROWS_AS(semantic::importTGeo(nullptr), nodehammer::Error);
}
