# ── Position-independent-code probe ───────────────────────────────────────────
# Guards the CMAKE_POSITION_INDEPENDENT_CODE setting in the top-level
# CMakeLists. A static archive links into an executable whether or not its
# objects are position independent, so nothing in a normal build notices when
# PIC regresses — the failure surfaces much later, at the first link that
# actually produces a shared object (the installed shared library, or the
# Python extension, which links the *static* library into a .so).
#
# The probe is that link, done early: an otherwise empty MODULE library that
# pulls in the whole nodehammer archive, so every object — plus every
# dependency object those objects reference — goes through the same relocation
# checks a real .so link applies. A non-PIC object fails the build here with
#   relocation R_X86_64_32 against `.rodata' can not be used when making a
#   shared object; recompile with -fPIC
# which is also how the FetchContent flatbuffers archive used to fail: it sets
# POSITION_INDEPENDENT_CODE only under tests/fuzzer/, never for the library.
#
# ELF-only on purpose. On Mach-O PIC is the default and non-PIC codegen is not
# reachable through the flags this project uses; on Windows and under
# Emscripten the concept does not apply. Enabling it there would run a link
# that cannot fail, which is worse than not running it.
#
# Off by default — it costs a full-archive link. The Linux x86_64 CI job turns
# it on; locally, `cmake --preset conan-relwithdebinfo -DNODEHAMMER_PIC_CHECK=ON`
# then `cmake --build ... --target nodehammer_pic_probe`.

if(NODEHAMMER_PIC_CHECK)
    if(NOT UNIX OR APPLE OR EMSCRIPTEN)
        message(STATUS "NODEHAMMER_PIC_CHECK: skipped (ELF targets only)")
    else()
        add_library(nodehammer_pic_probe MODULE ${CMAKE_CURRENT_LIST_DIR}/pic_probe.cpp)

        # One archive to name, since the lua front-end is compiled into
        # nodehammer_lib rather than a library of its own. That also removes the
        # hazard this block used to work around: naming both a leaf target and
        # the core it links PUBLIC meant CMake emitted the core twice, which is
        # harmless for ordinary archive semantics but fatal under
        # --whole-archive, where every symbol in it collides with itself. Lua is
        # still covered — the bindings link it statically into the same .so, so
        # it carries the same PIC requirement.
        target_link_libraries(nodehammer_pic_probe PRIVATE nodehammer_lib)

        # WHOLE_ARCHIVE (CMake >= 3.24) is what makes this a real test: without
        # it the linker pulls nothing out of the archive, since the probe TU
        # references none of it, and the probe would link clean no matter what.
        set_property(TARGET nodehammer_pic_probe PROPERTY
            LINK_LIBRARY_OVERRIDE_nodehammer_lib WHOLE_ARCHIVE
        )

        message(STATUS "NODEHAMMER_PIC_CHECK: enabled (target nodehammer_pic_probe)")
    endif()
endif()
