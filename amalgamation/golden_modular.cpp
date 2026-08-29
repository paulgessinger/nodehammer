// The modular half of the golden comparison.
//
// The same program against the ordinary build: the installed public header and
// the library behind it. Byte-for-byte agreement with the amalgamated half is
// the claim the amalgamation makes and had, until this test, no way to check --
// the smoke tests prove the generated header compiles and runs, not that it
// computes the same answer.

#include <nodehammer/semantic_scene.hpp>

#include "golden_body.hpp"

int main(int argc, char **argv) { return goldenMain(argc, argv); }
