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

        # Only the leaf target is named: nodehammer_lua links nodehammer_lib
        # PUBLIC, and CMake does not deduplicate an explicit entry against the
        # transitive one — harmless for ordinary archive semantics, fatal under
        # --whole-archive, where the archive is pulled twice and every symbol in
        # it collides with itself. The bindings will link lua statically into
        # the same .so, so it is subject to the same PIC requirement.
        if(TARGET nodehammer_lua)
            target_link_libraries(nodehammer_pic_probe PRIVATE nodehammer_lua)
        else()
            target_link_libraries(nodehammer_pic_probe PRIVATE nodehammer_lib)
        endif()

        # WHOLE_ARCHIVE (CMake >= 3.24) is what makes this a real test: without
        # it the linker pulls nothing out of the archives, since the probe TU
        # references none of it, and the probe would link clean no matter what.
        # Set as an override rather than a $<LINK_LIBRARY:...> genex on the link
        # itself so it also reaches nodehammer_lib, which arrives transitively.
        set_property(TARGET nodehammer_pic_probe PROPERTY
            LINK_LIBRARY_OVERRIDE_nodehammer_lib WHOLE_ARCHIVE
        )
        if(TARGET nodehammer_lua)
            set_property(TARGET nodehammer_pic_probe PROPERTY
                LINK_LIBRARY_OVERRIDE_nodehammer_lua WHOLE_ARCHIVE
            )
        endif()

        message(STATUS "NODEHAMMER_PIC_CHECK: enabled (target nodehammer_pic_probe)")
    endif()
endif()
