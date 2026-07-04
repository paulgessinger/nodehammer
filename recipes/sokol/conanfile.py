from conan import ConanFile
from conan.tools.files import copy, get, replace_in_file
import os


class SokolConan(ConanFile):
    name = "sokol"
    description = "Single-header cross-platform C/C++ libraries."
    license = "Zlib"
    homepage = "https://github.com/floooh/sokol"
    url = "https://github.com/floooh/sokol"
    topics = ("graphics", "single-header", "webgpu", "opengl", "metal", "d3d11")

    package_type = "header-library"
    no_copy_source = True

    def source(self):
        get(self, **self.conan_data["sources"][str(self.version)], strip_root=True)

        # emdawnwebgpu's JS wrapper rejects setVertexBuffer(slot, null, 0, 0).
        # Keep nodehammer's existing workaround in the packaged header so every
        # consumer sees the same fixed sokol_gfx.h without configure-time edits.
        replace_in_file(
            self,
            os.path.join(self.source_folder, "sokol_gfx.h"),
            "wgpuRenderPassEncoderSetVertexBuffer(_sg.wgpu.rpass_enc, slot, 0, 0, 0)",
            "((void)0)  /* nodehammer: emdawnwebgpu rejects null vb */",
        )

    def package(self):
        copy(self, "sokol_*.h", self.source_folder, os.path.join(self.package_folder, "include"))
        copy(self, "*.h", os.path.join(self.source_folder, "util"), os.path.join(self.package_folder, "include", "util"))
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "sokol")
        self.cpp_info.set_property("cmake_target_name", "sokol::headers")
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.libdirs = []
        self.cpp_info.bindirs = []
