#include <DD4hep/DetFactoryHelper.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

dd4hep::Ref_t createDetector(dd4hep::Detector &description, xml_h handle,
                             dd4hep::SensitiveDetector sensitive) {
    // Reproduce ALLEGRO's assumption: the plugin asks the default detector for
    // a constant, and then uses that same detector's geometry objects.
    auto &global = dd4hep::Detector::getInstance();
    if (&global != &description || global.constant<int>("global_detector_probe") != 42) {
        throw std::runtime_error("plugin did not receive the default detector");
    }
    std::puts("global detector plugin stdout");
    if (global.constant<int>("plugin_exit_code") != 0) {
        std::exit(global.constant<int>("plugin_exit_code"));
    }
    xml_det_t xml = handle;
    dd4hep::DetElement detector{xml.nameStr(), xml.id()};
    dd4hep::Volume volume{"GlobalBox", dd4hep::Box{10, 20, 30}, global.air()};
    sensitive.setType("tracker");
    volume.setSensitiveDetector(sensitive);
    detector.setPlacement(global.pickMotherVolume(detector).placeVolume(volume));
    return detector;
}

} // namespace

DECLARE_DETELEMENT(Nodehammer_GlobalDetectorProbe, createDetector)
