// No #pragma once: like stb_image_write.h itself, a translation unit may include it again after
// defining STB_IMAGE_WRITE_IMPLEMENTATION (e.g. after zgal/Main.h has included it), which compiles
// the implementation then.

/*
 stb_image_write, without its warnings. Its HDR writer uses sprintf, which macOS's SDK deprecates,
 so including it directly warns in the one translation unit defining the implementation:

    #define STB_IMAGE_WRITE_IMPLEMENTATION
    #include "util/StbImageWrite.h"

 Needs the wrapper's third_party/stb on the include path, like <stb_image.h>.
 */

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <stb_image_write.h>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
