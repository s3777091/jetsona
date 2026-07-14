#include "lvgl_image.h"
#include "esp_log.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

#define TAG "lvgl_image"

static bool isPng(const void *d) {
    const uint8_t *p = (const uint8_t *)d;
    return p && p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47;
}
static bool isJpeg(const void *d) {
    const uint8_t *p = (const uint8_t *)d;
    return p && p[0] == 0xFF && p[1] == 0xD8;
}
static bool isGif(const void *d) {
    const uint8_t *p = (const uint8_t *)d;
    return p && p[0] == 0x47 && p[1] == 0x49 && p[2] == 0x46 && p[3] == 0x38;
}

LvglRawImage::LvglRawImage(void *data, size_t size) : data_(data), size_(size) {
    std::memset(&image_dsc_, 0, sizeof(image_dsc_));
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = LV_COLOR_FORMAT_RAW;  // PNG/JPG/GIF decoders identify format by signature
    image_dsc_.data_size = (uint32_t)size;
    image_dsc_.data = (const uint8_t *)data;
}

LvglRawImage::~LvglRawImage() {
    if (data_) free(data_);
}

bool LvglRawImage::IsGif() const { return isGif(data_); }

LvglAllocatedImage::LvglAllocatedImage(void *data, size_t size) : data_(data) {
    std::memset(&image_dsc_, 0, sizeof(image_dsc_));
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = LV_COLOR_FORMAT_NATIVE;
    image_dsc_.data_size = (uint32_t)size;
    image_dsc_.data = (const uint8_t *)data;
}

LvglAllocatedImage::LvglAllocatedImage(void *data, size_t size, int width, int height,
                                       int /*stride*/, int color_format) : data_(data) {
    std::memset(&image_dsc_, 0, sizeof(image_dsc_));
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.w = (uint32_t)width;
    image_dsc_.header.h = (uint32_t)height;
    image_dsc_.header.cf = (lv_color_format_t)color_format;
    image_dsc_.data_size = (uint32_t)size;
    image_dsc_.data = (const uint8_t *)data;
}

LvglAllocatedImage::~LvglAllocatedImage() {
    if (data_) free(data_);
}

static bool readFile(const std::string &path, void **out, size_t *size) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    void *buf = std::malloc((size_t)sz);
    if (!buf) { std::fclose(f); return false; }
    if (std::fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        std::free(buf); std::fclose(f); return false;
    }
    std::fclose(f);
    *out = buf;
    *size = (size_t)sz;
    return true;
}

std::unique_ptr<LvglImage> LvglImageFromFile(const std::string &path) {
    void *data = nullptr;
    size_t size = 0;
    if (!readFile(path, &data, &size)) {
        ESP_LOGW(TAG, "image not found: %s", path.c_str());
        return nullptr;
    }
    if (!(isPng(data) || isJpeg(data) || isGif(data))) {
        ESP_LOGW(TAG, "unrecognized image magic in %s", path.c_str());
        free(data);
        return nullptr;
    }
    return std::unique_ptr<LvglImage>(new LvglRawImage(data, size));
}