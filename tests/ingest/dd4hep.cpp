#include <nodehammer/dd4hep.hpp>
#include <nodehammer/nhb.hpp>

#include <DD4hep/Detector.h>

#include <iostream>

int main(int argc, char **argv) {
    if (argc != 2) {
        return 1;
    }
    try {
        auto detector = dd4hep::Detector::make_unique("");
        detector->fromCompact(argv[1]);
        const auto result = nodehammer::fromDD4hep(*detector);
        return result.diags.hasErrors() || result.scene.nodeCount() != 2 ||
                       nodehammer::toNhb(result.scene).empty()
                   ? 1
                   : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
