#include "display/core/lvgl_image.h"
#include "esp_log.h"

/* Bundled decoders, driven directly for LvglImageFromFileFit: TJPGD for a
 * one-shot full-frame JPEG decode (LVGL's patched copy already emits the
 * B,G,R byte order LV_COLOR_FORMAT_RGB888 expects) and lodepng for PNG. */
#include <src/libs/lodepng/lodepng.h>
#include <src/libs/tjpgd/tjpgd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>

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

/* Parse pixel dimensions from an encoded PNG/JPEG header. LVGL's TJPGD decoder
 * reads width/height straight from this descriptor in its info stage for an
 * in-memory (LV_IMAGE_SRC_VARIABLE) JPEG -- unlike lodepng, which parses the PNG
 * IHDR itself. If we leave them at 0 the JPEG is treated as 0x0 and nothing is
 * drawn, so populate them here. Returns false when the header can't be parsed. */
static bool parseEncodedDims(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h) {
    if (!d || n < 24) return false;
    if (d[0] == 0x89 && d[1] == 0x50) {  // PNG: IHDR width/height at bytes 16..23
        *w = ((uint32_t)d[16] << 24) | ((uint32_t)d[17] << 16) |
             ((uint32_t)d[18] << 8) | d[19];
        *h = ((uint32_t)d[20] << 24) | ((uint32_t)d[21] << 16) |
             ((uint32_t)d[22] << 8) | d[23];
        return *w && *h;
    }
    if (d[0] != 0xff || d[1] != 0xd8) return false;  // not JPEG
    size_t p = 2;
    while (p + 8 < n) {
        if (d[p] != 0xff) { ++p; continue; }
        while (p < n && d[p] == 0xff) ++p;  // skip fill bytes
        if (p >= n) break;
        const uint8_t marker = d[p++];
        if (marker == 0xd8 || marker == 0xd9) continue;
        if (p + 2 > n) break;
        const size_t len = ((size_t)d[p] << 8) | d[p + 1];
        if (len < 2 || p + len > n) break;
        const bool sof = (marker >= 0xc0 && marker <= 0xc3) ||
                         (marker >= 0xc5 && marker <= 0xc7) ||
                         (marker >= 0xc9 && marker <= 0xcb) ||
                         (marker >= 0xcd && marker <= 0xcf);
        if (sof && len >= 7) {
            *h = ((uint32_t)d[p + 3] << 8) | d[p + 4];
            *w = ((uint32_t)d[p + 5] << 8) | d[p + 6];
            return *w && *h;
        }
        p += len;
    }
    return false;
}

LvglRawImage::LvglRawImage(void *data, size_t size) : data_(data), size_(size) {
    std::memset(&image_dsc_, 0, sizeof(image_dsc_));
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = LV_COLOR_FORMAT_RAW;  // PNG/JPG/GIF decoders identify format by signature
    image_dsc_.data_size = (uint32_t)size;
    image_dsc_.data = (const uint8_t *)data;
    uint32_t w = 0, h = 0;
    if (parseEncodedDims((const uint8_t *)data, size, &w, &h)) {
        image_dsc_.header.w = w;
        image_dsc_.header.h = h;
    }
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
                                       int stride, int color_format) : data_(data) {
    std::memset(&image_dsc_, 0, sizeof(image_dsc_));
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.w = (uint32_t)width;
    image_dsc_.header.h = (uint32_t)height;
    image_dsc_.header.stride = (uint32_t)stride;
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

/* ---- Full-frame decode + cover-fit resize (LvglImageFromFileFit) ---- */

namespace {

struct TjpgdSource {
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint8_t *out;       // width*height*3, LVGL RGB888 byte order (B,G,R)
    unsigned width;
    unsigned height;
};

size_t TjpgdRead(JDEC *jd, uint8_t *buf, size_t len) {
    auto *src = static_cast<TjpgdSource *>(jd->device);
    const size_t remain = src->size - src->pos;
    if (len > remain) len = remain;
    if (buf) std::memcpy(buf, src->data + src->pos, len);
    src->pos += len;
    return len;
}

int TjpgdWrite(JDEC *jd, void *bitmap, JRECT *rect) {
    auto *src = static_cast<TjpgdSource *>(jd->device);
    if (rect->right >= src->width || rect->bottom >= src->height) return 0;
    const uint8_t *pix = static_cast<const uint8_t *>(bitmap);
    const size_t run = (size_t)(rect->right - rect->left + 1) * 3;
    for (unsigned y = rect->top; y <= rect->bottom; ++y) {
        std::memcpy(src->out + ((size_t)y * src->width + rect->left) * 3, pix, run);
        pix += run;
    }
    return 1; // continue decoding
}

/* One-shot TJPGD decode (baseline JPEG only) into a tightly packed RGB888
 * buffer owned by the caller (free()). nullptr on any decoder error, e.g. a
 * progressive JPEG (JDR_FMT3). */
uint8_t *DecodeJpegRgb888(const uint8_t *data, size_t size,
                          unsigned *w, unsigned *h) {
    /* 8 KB pool: TJPGD needs ~3.5 KB for baseline 4:2:0; LVGL's own decoder
     * runs with 4 KB, doubled here for headroom at trivial cost. The JDEC
     * keeps pointers into the pool, so it must outlive jd_decomp. */
    constexpr size_t kPoolSize = 8192;
    std::unique_ptr<uint8_t[]> pool(new uint8_t[kPoolSize]);
    TjpgdSource src{data, size, 0, nullptr, 0, 0};
    JDEC jd;
    if (jd_prepare(&jd, TjpgdRead, pool.get(), kPoolSize, &src) != JDR_OK)
        return nullptr;
    if (jd.width == 0 || jd.height == 0 || jd.width > 4096 || jd.height > 4096)
        return nullptr;
    src.width = jd.width;
    src.height = jd.height;
    src.out = (uint8_t *)std::malloc((size_t)jd.width * jd.height * 3);
    if (!src.out) return nullptr;
    if (jd_decomp(&jd, TjpgdWrite, 0) != JDR_OK) {
        std::free(src.out);
        return nullptr;
    }
    *w = jd.width;
    *h = jd.height;
    return src.out;
}

/* Fixed-point bilinear resample of a tightly packed image (3/4 bytes per
 * pixel). Returns a malloc'd buffer of dw*dh*channels. */
uint8_t *ResizePixels(const uint8_t *src, unsigned sw, unsigned sh,
                      unsigned dw, unsigned dh, int channels) {
    uint8_t *dst = (uint8_t *)std::malloc((size_t)dw * dh * (size_t)channels);
    if (!dst) return nullptr;
    for (unsigned y = 0; y < dh; ++y) {
        const uint32_t fy = (uint32_t)(((uint64_t)y * sh << 8) / dh);
        const uint32_t y0 = fy >> 8;
        const uint32_t wy = fy & 0xff;
        const uint32_t y1 = std::min(y0 + 1, sh - 1);
        for (unsigned x = 0; x < dw; ++x) {
            const uint32_t fx = (uint32_t)(((uint64_t)x * sw << 8) / dw);
            const uint32_t x0 = fx >> 8;
            const uint32_t wx = fx & 0xff;
            const uint32_t x1 = std::min(x0 + 1, sw - 1);
            const uint8_t *p00 = src + ((size_t)y0 * sw + x0) * channels;
            const uint8_t *p01 = src + ((size_t)y0 * sw + x1) * channels;
            const uint8_t *p10 = src + ((size_t)y1 * sw + x0) * channels;
            const uint8_t *p11 = src + ((size_t)y1 * sw + x1) * channels;
            uint8_t *o = dst + ((size_t)y * dw + x) * channels;
            for (int c = 0; c < channels; ++c) {
                const uint32_t top = p00[c] * (256 - wx) + p01[c] * wx;
                const uint32_t bottom = p10[c] * (256 - wx) + p11[c] * wx;
                o[c] = (uint8_t)((top * (256 - wy) + bottom * wy) >> 16);
            }
        }
    }
    return dst;
}

} // namespace

std::unique_ptr<LvglImage> LvglImageFromFileFit(const std::string &path, int box_px) {
    if (box_px <= 0) return nullptr;
    void *raw = nullptr;
    size_t raw_size = 0;
    if (!readFile(path, &raw, &raw_size)) {
        ESP_LOGW(TAG, "image not found: %s", path.c_str());
        return nullptr;
    }

    unsigned w = 0, h = 0;
    uint8_t *pixels = nullptr;
    int channels = 0;
    lv_color_format_t cf = LV_COLOR_FORMAT_RGB888;
    if (isJpeg(raw)) {
        pixels = DecodeJpegRgb888((const uint8_t *)raw, raw_size, &w, &h);
        channels = 3;
    } else if (isPng(raw)) {
        unsigned char *rgba = nullptr;
        if (lodepng_decode32(&rgba, &w, &h, (const unsigned char *)raw,
                             raw_size) == 0 && rgba) {
            /* lodepng emits R,G,B,A; LVGL ARGB8888 stores B,G,R,A in memory. */
            const size_t count = (size_t)w * h;
            pixels = (uint8_t *)std::malloc(count * 4);
            if (pixels) {
                for (size_t i = 0; i < count; ++i) {
                    pixels[i * 4 + 0] = rgba[i * 4 + 2];
                    pixels[i * 4 + 1] = rgba[i * 4 + 1];
                    pixels[i * 4 + 2] = rgba[i * 4 + 0];
                    pixels[i * 4 + 3] = rgba[i * 4 + 3];
                }
            }
            /* This tree's lodepng allocates through lv_malloc (see
             * LODEPNG_COMPILE_ALLOCATORS in lodepng.c). */
            lv_free(rgba);
        }
        channels = 4;
        cf = LV_COLOR_FORMAT_ARGB8888;
    }
    free(raw);
    if (!pixels || w == 0 || h == 0) {
        ESP_LOGW(TAG, "cannot fully decode %s", path.c_str());
        std::free(pixels);
        return nullptr;
    }

    const unsigned shorter = std::min(w, h);
    unsigned dw = w;
    unsigned dh = h;
    if ((int)shorter != box_px) {
        dw = std::max<unsigned>(1, (unsigned)((uint64_t)w * (unsigned)box_px / shorter));
        dh = std::max<unsigned>(1, (unsigned)((uint64_t)h * (unsigned)box_px / shorter));
        uint8_t *scaled = ResizePixels(pixels, w, h, dw, dh, channels);
        std::free(pixels);
        if (!scaled) return nullptr;
        pixels = scaled;
    }
    return std::unique_ptr<LvglImage>(new LvglAllocatedImage(
        pixels, (size_t)dw * dh * (size_t)channels, (int)dw, (int)dh,
        (int)dw * channels, cf));
}
