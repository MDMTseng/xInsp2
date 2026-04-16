// Single translation unit that instantiates stb_image_write.
// Keeping the implementation pragmas in their own file means the inline
// header can be included freely elsewhere without multiple-definition
// linker errors.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
