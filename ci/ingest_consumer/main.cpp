#ifdef CONSUMER_MINIMAL_HEADERS
#if __has_include(<nodehammer/io.hpp>) || __has_include(<nodehammer/config.hpp>) || __has_include(<nodehammer/render_scene.hpp>)
#error "The ingestion source target exposed full-library headers"
#endif
#endif
#include <nodehammer/nhb.hpp>
#include <nodehammer/version.hpp>

#include <iostream>

#ifdef CONSUMER_WITH_TGEO
#include <TGeoManager.h>
#include <TGeoMaterial.h>
#include <TGeoMedium.h>
#include <TGeoVolume.h>
#include <nodehammer/tgeo.hpp>
#endif

int main() {
    if (nodehammer::version() != nodehammer::VERSION) {
        std::cerr << "version mismatch: headers=" << nodehammer::VERSION
                  << ", library=" << nodehammer::version() << '\n';
        return 1;
    }
#ifdef CONSUMER_WITH_TGEO
    TGeoManager manager("consumer", "existing experiment geometry");
    auto *material = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *medium = new TGeoMedium("vacuum", 1, material);
    auto *world = manager.MakeBox("world", medium, 100, 100, 100);
    manager.SetTopVolume(world);
    manager.CloseGeometry();
    const auto result = nodehammer::fromTGeo(manager);
    return result.diags.hasErrors() || result.scene.nodeCount() != 1 ||
                   nodehammer::toNhb(result.scene).empty()
               ? 1
               : 0;
#else
    try {
        (void)nodehammer::toNhb(nodehammer::SemanticScene{});
    } catch (const nodehammer::Error &) {
        return 0;
    }
    std::cerr << "toNhb accepted an invalid SemanticScene instead of throwing Error\n";
    return 1;
#endif
}
