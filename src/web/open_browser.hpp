#pragma once

// Handing a URL to whatever the desktop uses to open one.
//
// In C++ rather than in Python, even though `webbrowser` is the nicer
// implementation: the CLI needs this with no interpreter anywhere near it, a
// remote static build has no Python at all, and two implementations of "open a
// URL" is exactly the kind of thing that surfaces later as one platform
// behaving differently for a reason nobody can find.

#include <string>

namespace nodehammer::web {

/// Ask the desktop to open `url`. False if there was nothing to ask.
///
/// Not whether the browser *appeared* — no platform tells us that, and a
/// headless box legitimately has nothing to show. The caller prints the URL
/// regardless, so a false here costs the reader a copy-paste rather than the
/// feature.
bool openInBrowser(const std::string &url);

} // namespace nodehammer::web
