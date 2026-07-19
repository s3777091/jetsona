#ifndef LVGL_IMAGE_H
#define LVGL_IMAGE_H

#include <lvgl.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

/* Wrap an lv_img_dsc_t. On Linux the bytes come from the filesystem and the
 * image is decoded by LVGL's built-in PNG/JPG/GIF decoders. */
class LvglImage {
public:
    virtual ~LvglImage() = default;
    virtual const lv_img_dsc_t *image_dsc() const = 0;
    virtual bool IsGif() const { return false; }
};

/* Raw PNG/JPG/GIF bytes decoded by LVGL's built-in decoders. Owns its buffer. */
class LvglRawImage : public LvglImage {
public:
    LvglRawImage(void *data, size_t size);
    virtual ~LvglRawImage();
    virtual const lv_img_dsc_t *image_dsc() const override { return &image_dsc_; }
    virtual bool IsGif() const override;

private:
    void *data_ = nullptr;
    size_t size_ = 0;
    lv_img_dsc_t image_dsc_;
};

/* cbin format is ESP-asset-specific; stubbed for the Linux port. */
class LvglCBinImage : public LvglImage {
public:
    LvglCBinImage(void * /*data*/) {}
    virtual const lv_img_dsc_t *image_dsc() const override { return &image_dsc_; }
private:
    lv_img_dsc_t image_dsc_ = {};
};

class LvglSourceImage : public LvglImage {
public:
    LvglSourceImage(const lv_img_dsc_t *image_dsc) : image_dsc_(image_dsc) {}
    virtual const lv_img_dsc_t *image_dsc() const override { return image_dsc_; }
private:
    const lv_img_dsc_t *image_dsc_;
};

class LvglAllocatedImage : public LvglImage {
public:
    LvglAllocatedImage(void *data, size_t size);
    LvglAllocatedImage(void *data, size_t size, int width, int height, int stride, int color_format);
    virtual ~LvglAllocatedImage();
    virtual const lv_img_dsc_t *image_dsc() const override { return &image_dsc_; }
private:
    void *data_ = nullptr;
    lv_img_dsc_t image_dsc_;
};

/* Load an image file (PNG/JPG/GIF) from disk into an owning LvglRawImage. */
std::unique_ptr<LvglImage> LvglImageFromFile(const std::string &path);

/* Decode a JPEG/PNG file completely into raw pixels and cover-fit it so the
 * SHORTER side equals box_px (aspect ratio preserved; the longer side
 * overflows the box and is meant to be clipped by the host object). The
 * returned descriptor is fully decoded (RGB888 for JPEG, ARGB8888 for PNG):
 * it draws as a plain blit with scale=256, is never re-decoded per frame and
 * needs no LVGL transform.
 *
 * Why this exists: LVGL 9.2's built-in TJPGD decoder streams JPEG files and
 * in-memory JPEGs MCU-by-MCU (get_area). Combining that streaming path with
 * lv_image_set_scale intersects untransformed decode strips with the
 * transformed screen area — covers render blank (the music "missing artwork"
 * bug) and the sw renderer can read past the strip buffer (the album
 * pull-to-refresh crash). Decoding up-front side-steps that decoder entirely.
 * Returns nullptr when the file is missing or not a baseline JPEG/PNG. */
std::unique_ptr<LvglImage> LvglImageFromFileFit(const std::string &path, int box_px);

#endif