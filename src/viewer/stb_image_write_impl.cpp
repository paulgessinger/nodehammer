// The single translation unit that compiles stb_image_write's implementation.
// Keep it isolated so the rest of the viewer can include <stb_image_write.h>
// (declarations only) without pulling in the implementation more than once.
#define STB_IMAGE_WRITE_IMPLEMENTATION
// PNG is the only format the viewer's screenshot export needs; dropping the
// others trims the implementation that gets compiled in.
#define STBI_WRITE_NO_STDIO // we encode to a memory buffer, never to a FILE*
#include <stb_image_write.h>
