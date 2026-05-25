#include "png_export_readback.hpp"

// PNG-export GPU readback for the D3D11 backend is not implemented yet. The
// shape is known — sokol exposes the device/context (sg_d3d11_device /
// sg_d3d11_device_context) and the source texture (sg_d3d11_query_image_info);
// the readback would CopyResource into a STAGING texture (D3D11_USAGE_STAGING,
// CPU_ACCESS_READ), Map it, de-pad rows by RowPitch and swizzle BGRA→RGBA — but
// there's no Windows/D3D11 environment here to write and verify it against.
//
// Gate it behind #error so a D3D11 build can't silently ship a broken "Export
// PNG" button: implement and test the readback, then remove this stub from the
// build (see src/viewer/CMakeLists.txt) and the matching note in
// docs/png-export.md.
#error                                                                                             \
    "PNG export readback is not implemented for the D3D11 backend yet (Metal / WebGPU / WebGL2 are wired up)."
