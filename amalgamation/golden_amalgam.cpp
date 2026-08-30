// The amalgamated half of the golden comparison.
//
// Reaches nodehammer only through the generated single header — no
// nodehammer_lib, no `src/`, no `include/`, exactly as the shipped artifact is
// consumed. Everything else it does is in golden_body.hpp, shared with the
// modular half.

#define NH_IMPLEMENTATION
#include "nodehammer_connect.h"

#include "golden_body.hpp"

int main(int argc, char **argv) { return goldenMain(argc, argv); }
