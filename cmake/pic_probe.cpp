// Payload-free translation unit for the NODEHAMMER_PIC_CHECK probe module
// (cmake/PicProbe.cmake). The thing under test is everything the linker pulls
// in around it — the whole nodehammer archive — not this file; CMake just
// requires a MODULE library to have at least one source, and a linker requires
// at least one symbol to be worth emitting.
extern "C" int nodehammer_pic_probe_marker(void) {
    return 0;
}
