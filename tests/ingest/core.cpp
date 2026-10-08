#include <nodehammer/nhb.hpp>

#include <api/handles_semantic.hpp>
#include <ir/expanded/scene.hpp>

#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

int main(int argc, char **argv) {
    try {
        namespace sem = nodehammer::ir::semantic;
        nodehammer::ir::expanded::Scene scene;
        const auto mat = scene.nextMaterialId();
        scene.materials[mat] = {mat, "vacuum", std::nullopt, 0.0};
        const auto shape = scene.nextShapeId();
        scene.shapes[shape] = {shape, sem::BoxShape{1.0, 2.0, 3.0}};
        const auto lv = scene.nextLogVolId();
        scene.logVols[lv] = {lv, "box", shape, mat};

        const auto root = scene.nextNodeId();
        const auto child = scene.nextNodeId();
        const auto leaf = scene.nextNodeId();
        for (const auto id : {root, child, leaf}) {
            nodehammer::ir::expanded::Node node;
            node.id = id;
            node.logVolId = lv;
            scene.nodes[id] = node;
        }
        scene.rootId = root;
        scene.nodes[root].children = {child};
        scene.nodes[child].parentId = root;
        scene.nodes[child].children = {leaf};
        scene.nodes[leaf].parentId = child;

        // A translated parent rotated 90 degrees around Z, followed by a
        // translated child. Unlike a .nhb comparison this observes the computed
        // world matrices, including composition below a non-identity parent.
        auto &parent = scene.nodes[child].localTransform;
        parent[0] = {0, 1, 0, 0};
        parent[1] = {-1, 0, 0, 0};
        parent[3] = {10, 20, 30, 1};
        scene.nodes[leaf].localTransform[3] = {2, 3, 4, 1};
        scene.computeWorldTransforms();
        const auto &world = scene.nodes.at(leaf).worldTransform;
        require(std::abs(world[3][0] - 7.0) < 1e-12, "composed X");
        require(std::abs(world[3][1] - 22.0) < 1e-12, "composed Y");
        require(std::abs(world[3][2] - 34.0) < 1e-12, "composed Z");
        require(world[0][1] == 1.0 && world[1][0] == -1.0, "composed rotation");

        const auto handle = nodehammer::api::asHandle(std::move(scene));
        require(handle.nodeCount() == 3 && handle.logVolCount() == 1, "scene counts");
        const auto bytes = nodehammer::toNhb(handle);
        require(bytes.size() > 8, "serialized bytes");
        require(static_cast<char>(bytes[4]) == 'N' && static_cast<char>(bytes[5]) == 'H' &&
                    static_cast<char>(bytes[6]) == 'S' && static_cast<char>(bytes[7]) == '8',
                "semantic file identifier");
        if (argc != 2) {
            throw std::runtime_error("expected output fixture path");
        }
        std::ofstream output(argv[1], std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        require(!output.fail(), "write transport fixture");
        bool caught = false;
        try {
            (void)nodehammer::toNhb(nodehammer::SemanticScene{});
        } catch (const nodehammer::Error &) {
            caught = true;
        }
        require(caught, "empty handle error");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
