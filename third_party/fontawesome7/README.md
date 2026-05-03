# Vendored Font Awesome 7

Two pieces are needed to render Font Awesome icons in the ImGui UI: a way to
**name** glyphs from C++, and the actual **glyph outlines** ImGui hands to its
font rasterizer. Font Awesome ships those as separate artifacts maintained by
different projects, so we vendor one of each. They're tiny, stable, and have no
build-time machinery, so we keep them as plain checked-in files instead of
fetching them at configure time.

## The two files

### `IconsFontAwesome7.h` — *names → bytes*

A header full of `#define ICON_FA_<NAME> "<utf8 bytes>"` macros, one per icon.
Example:

```c
#define ICON_FA_CIRCLE_INFO "\xef\x81\x9a"   // U+f05a
```

It contains **no font data** — just the mapping from human-readable glyph
names to the UTF-8 codepoints in Font Awesome's Private Use Area. Without it,
we'd have to write `"\xef\x81\x9a"` everywhere we wanted an info icon and
hope we typed it right.

The header also defines the codepoint range covered (`ICON_MIN_FA`,
`ICON_MAX_16_FA`), which `icon_font::initialize()` passes to ImGui so the
atlas only rasterizes glyphs that actually exist in the font.

- Source: [juliettef/IconFontCppHeaders](https://github.com/juliettef/IconFontCppHeaders)
  (a community project — *not* part of upstream Font Awesome).
- Generated upstream from Font Awesome's `metadata/icons.yml`.
- License: Zlib — see [`LICENSES/Zlib.txt`](../../LICENSES/Zlib.txt).

### `fa-solid-900.h` — *bytes → pixels*

A C array containing the entire Font Awesome 7 Free Solid font, LZ4-compressed
and embedded as `fa_solid_900_compressed_data[]`. ImGui decompresses this at
startup, hands it to stb_truetype, and rasterizes the requested glyphs into
the font atlas.

Without this, the macros above would resolve to UTF-8 strings that ImGui
couldn't draw — just empty boxes.

We generated this file ourselves from the official Font Awesome release using
the small tool that ships with imgui:

```bash
# Download FA7 Free desktop release and extract the solid OTF
curl -LO https://github.com/FortAwesome/Font-Awesome/releases/download/7.2.0/fontawesome-free-7.2.0-desktop.zip
unzip -j fontawesome-free-7.2.0-desktop.zip 'fontawesome-free-7.2.0-desktop/otfs/Font Awesome 7 Free-Solid-900.otf'

# Build imgui's font compressor (lives in the imgui source tree)
c++ -std=c++17 -O2 -o binary_to_compressed_c \
    build/RelWithDebInfo/_deps/imgui-src/misc/fonts/binary_to_compressed_c.cpp

# Convert the OTF to a C header
./binary_to_compressed_c "Font Awesome 7 Free-Solid-900.otf" fa_solid_900 \
    > third_party/fontawesome7/fa-solid-900.h
```

Despite ImGui's `AddFontFromMemoryCompressedTTF` API name, the loader handles
both TTF and OTF — stb_truetype parses CFF outlines just fine. We use the OTF
because that's what ships in the official release.

- Source: [Font Awesome 7.2.0 Free desktop release](https://github.com/FortAwesome/Font-Awesome/releases/tag/7.2.0).
- License: SIL OFL 1.1 — see [`LICENSES/OFL-1.1.txt`](../../LICENSES/OFL-1.1.txt).

## Why vendor instead of FetchContent?

- `IconsFontAwesome7.h` is a single header. Pulling a whole git repo for one
  file is overkill, and it churns rarely — only when Font Awesome adds icons.
- `fa-solid-900.h` is *generated*, not source. FetchContent can't produce it
  without us also building the `binary_to_compressed_c` tool at configure time
  and running it against a downloaded archive — far more complexity than
  checking in 250 KB of post-processed bytes.

## Updating

1. Replace `IconsFontAwesome7.h` from upstream `juliettef/IconFontCppHeaders`.
2. Re-run the `binary_to_compressed_c` recipe above against the latest FA7
   release OTF.
3. Bump the version in this README.

## How it's wired

CMake exposes the directory as the `nh_fontawesome7` INTERFACE library
(declared in [`cmake/Dependencies.cmake`](../../cmake/Dependencies.cmake)),
linked into the viewer static lib in [`src/viewer/CMakeLists.txt`](../../src/viewer/CMakeLists.txt).

```cpp
#include "ui/icon_font.hpp"   // brings in ICON_FA_* macros

ImGui::Text(ICON_FA_CIRCLE_INFO " hello");
```

The font is installed into the ImGui atlas by `ui::icon_font::initialize()`,
called once at startup from `App::Impl::onInit`.
