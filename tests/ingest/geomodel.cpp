#include <fstream>
#include <iostream>
#include <ir/fb/semantic/importer.hpp>
#include <ir/geomodel/semantic/importer.hpp>
#include <ir/semantic/flatbuffer.hpp>
int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            std::cerr << "usage: nodehammer_geomodel_check input.db output.nhb\n";
            return 2;
        }
        auto result = nodehammer::ir::GeoModelImporter{}.import(argv[1]);
        for (const auto &d : result.diags)
            std::cerr << d.code << ": " << d.message << '\n';
        auto bytes = nodehammer::ir::semantic::sceneToBytes(result.scene);
        auto restored = nodehammer::ir::FlatBufferImporter::importFromBytes(argv[2], bytes);
        auto second = nodehammer::ir::semantic::sceneToBytes(restored.scene);
        if (bytes != second)
            throw std::runtime_error("NHB round-trip changed canonical bytes");
        std::ofstream output(argv[2], std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        if (!output)
            throw std::runtime_error("output write failed");
        std::cout << "occurrences=" << result.scene.nodeCount()
                  << " volumes=" << result.scene.logVols.size()
                  << " shapes=" << result.scene.shapes.size()
                  << " materials=" << result.scene.materials.size() << " bytes=" << bytes.size()
                  << " roundtrip=identical\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
