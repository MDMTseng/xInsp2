// Out-of-line implementation of host_api->read_image_file.
// Lives in its own TU so xi_image_pool.hpp doesn't need to drag
// stb_image.h into every consumer; the STB_IMAGE_IMPLEMENTATION
// macro is defined once in stb_impl.cpp.
//
// Static initialiser installs the function pointer into ImagePool
// so make_host_api hands it out. Tests that don't link this TU
// leave the slot null and read_image_file is unavailable to plugins
// they spawn — fine for in-process unit tests.

#include <xi/xi_image_pool.hpp>
#include <stb_image.h>
#include <cstring>

namespace xi {

static xi_image_handle read_image_file_impl(const char* path) {
    if (!path) return 0;
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(path, &w, &h, &ch, 0);
    if (!px) return 0;
    auto& pool = ImagePool::instance();
    xi_image_handle handle = pool.create(w, h, ch);
    if (!handle) { stbi_image_free(px); return 0; }
    if (uint8_t* dst = pool.data(handle)) {
        std::memcpy(dst, px, (size_t)w * h * ch);
    }
    stbi_image_free(px);
    return handle;
}

namespace {
struct InstallReadImageFile {
    InstallReadImageFile() {
        ImagePool::install_read_image_file(&read_image_file_impl);
    }
};
static InstallReadImageFile s_install;
}

} // namespace xi
