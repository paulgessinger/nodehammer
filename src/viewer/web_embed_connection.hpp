#pragma once

#include <viewer/viewer_connection.hpp>

#include <memory>

namespace nodehammer::viewer {

/// Web-only postMessage adapter. Native builds do not compile its implementation.
[[nodiscard]] std::shared_ptr<ViewerConnection> makeWebEmbedConnection();

} // namespace nodehammer::viewer
