#include <ir/tgeo/semantic/shared_importer.hpp>
namespace nodehammer::ir::semantic {
ir::ImportResult importTGeo(TGeoManager *manager, std::string sourceFile) {
    return detail::extractTGeoSource(manager, std::move(sourceFile)).result;
}
} // namespace nodehammer::ir::semantic
