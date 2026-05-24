#include "png_export_readback.hpp"

// PNG-export GPU readback is implemented for the backends the project can
// currently build and test: Metal (native macOS) and WebGPU / WebGL2 (web).
// D3D11, Vulkan and desktop GLCORE need their own readback path
// (ID3D11DeviceContext::CopyResource + Map, vkCmdCopyImageToBuffer, or
// glReadPixels via an FBO respectively). They are gated behind this #error so a
// new backend can't silently ship a broken "Export PNG" button — implement and
// test the readback, then remove this stub from the build.
#error                                                                                             \
    "PNG export readback is not implemented for this graphics backend yet (only Metal / WebGPU / WebGL2 are wired up)."
