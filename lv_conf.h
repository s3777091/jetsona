#ifndef LV_CONF_H
#define LV_CONF_H

/* clang-format off */

#include <stdint.h>

/*====================
   COLOR SETTINGS
====================*/
/* DS-02 uses RGB565. DRM/SDL backends will convert as needed. */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_CHROMA_KEY lv_color_hex(0xff00ff)
#define LV_COLOR_SCREEN_TRANSP 0

/*====================
   MEMORY
====================*/
/* Use the C library malloc/free/strlen on Linux. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB
#define LV_MEM_SIZE             (256U * 1024U)   /* only used if BUILTIN malloc; harmless otherwise */
#define LV_MEM_ADR              0
#define LV_MEM_BUF_MAX_NUM      16

/*====================
   OS / HAL
====================*/
/* LVGL thread-safety via pthreads: lv_lock()/lv_unlock() available. */
#define LV_USE_OS   LV_OS_PTHREAD

/*====================
   RENDERING
====================*/
#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW
    #define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
    /* MUST stay 1: TinyTTF (lv_tiny_ttf.c) rasterizes glyphs into a shared
     * cache with no locking, so >1 draw unit makes parallel text draws race —
     * glyphs come out missing/corrupted all over the UI (seen on-device with
     * 2 units). Revisit only if the text font moves to pre-rendered bitmaps. */
    #define LV_DRAW_SW_DRAW_UNIT_CNT 1
    /* Must stay NONE on the Nano: LVGL's NEON blend asm (lv_blend_neon.S) is
     * 32-bit ARM (.arch armv7a) and does not assemble on aarch64. */
    #define LV_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#endif
/* Draw threads rasterize TinyTTF glyphs (stb_truetype) on their own stack;
 * LVGL's 8 KB default is tight for that. Stack is virtual memory on Linux,
 * so 64 KB per draw thread costs nothing until actually touched. */
#define LV_DRAW_THREAD_STACK_SIZE (64 * 1024)
#define LV_USE_DRAW_DMA2D 0
#define LV_USE_DRAW_PXP 0
#define LV_USE_DRAW_VGLITE 0
#define LV_USE_DRAW_SDL 0

/*====================
   LOGGING
====================*/
#define LV_USE_LOG 1
#if LV_USE_LOG
    #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF 1
    #define LV_LOG_USE_ASSERT 1
#endif

/*====================
   ASSERTS
====================*/
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ_INTEGRITY 0

/*====================
   OTHERS
====================*/
#define LV_USE_ANIMIMG 1
/* App-switcher thumbnails: lv_snapshot_take renders a live overlay into a
 * draw buffer so background apps keep a preview card without staying drawn. */
#define LV_USE_SNAPSHOT 1
#define LV_USE_OBJX_NAME 0
#define LV_USE_OBJ_PROPERTY 0
#define LV_USE_OS_TASK 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_REFR_DEBUG 0
#define LV_USE_LAYER_DEBUG 0

/* 1: Enable buffer depth/size test on draw buf. */
#define LV_DRAW_BUF_STRIDE_ALIGN 64
/* LVGL requires draw buffers passed to lv_display_set_buffers to already be
 * aligned to LV_DRAW_BUF_ALIGN (asserted in lv_display.c). The fbdev driver
 * (lv_linux_fbdev.c) allocates its draw buffer with plain malloc(), which glibc
 * only guarantees to be 16-byte aligned on aarch64. 16 is also the natural
 * NEON alignment for the SW draw path, so use 16 here to satisfy the assertion
 * without patching the vendored driver. Row stride stays 64-byte aligned above. */
#define LV_DRAW_BUF_ALIGN 16

/* 1: Use NXP's PMU/PSA crypto for OTA. (not used) */
#define LV_USE_SYSMON 0

/*====================
   FONTS
====================*/
#define LV_FONT_DEFAULT &lv_font_montserrat_14
/* Built-in montserrat fonts available as fallbacks. */
#define LV_FONT_MONTSERRAT_8  1
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_38 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_42 1
#define LV_FONT_MONTSERRAT_44 1
#define LV_FONT_MONTSERRAT_46 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK 0
#define LV_FONT_UNSCII_8 0
#define LV_FONT_UNSCII_16 0

/* Declare built-in font symbols. The DS-02 UI references
 * BUILTIN_TEXT_FONT / BUILTIN_ICON_FONT; we map them to runtime
 * tiny_ttf fonts created in main (see src/app/fonts.h). */
#define LV_USE_TINY_TTF 1
#if LV_USE_TINY_TTF
    /* Allow tiny_ttf to read TTF files from the filesystem. */
    #define LV_TINY_TTF_FILE_SUPPORT 1
    #define LV_TINY_TTF_FILE_CACHE_SIZE 4096
#endif
#define LV_USE_GIF 0
#define LV_USE_BIN_DECODER 1
#define LV_USE_BMP 0
#define LV_USE_RLE 0
#define LV_USE_TJPGD 1   /* tiny jpeg decoder (built-in) */
#define LV_USE_LODEPNG 1  /* built-in PNG decoder (no libpng needed) */
#define LV_USE_LIBPNG 0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_FREETYPE 0
#define LV_USE_RLOTTIE 0
#define LV_USE_THORVG 0
#define LV_USE_FFMPEG 0
#define LV_USE_QRCODE 0
#define LV_USE_BARCODE 0
#define LV_USE_TINY_TTF 1

/*====================
   THEMES
====================*/
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK 0
    #define LV_THEME_DEFAULT_LIGHT 1
    #define LV_THEME_DEFAULT_FONT &lv_font_montserrat_14
#endif
#define LV_USE_THEME_SIMPLE 1
#define LV_USE_THEME_MONO 1

/*====================
   FILESYSTEM
====================*/
#define LV_USE_FS_STDIO 1
#if LV_USE_FS_STDIO
    #define LV_FS_STDIO_LETTER 'A'
    #define LV_FS_STDIO_BUFFER_SIZE 4096
    /* Bare relative paths with no "X:" drive prefix resolve to this letter.
     * Lets lv_tiny_ttf_create_file open "assets/fonts/arial.ttf" through the
     * stdio ('A') driver without per-callsite "A:" prefixing. LV_FS_STDIO_PATH
     * is empty, so the stdio driver fopen()s the path as-is (relative to CWD,
     * which is the build dir with assets/ copied next to the binary). */
    #define LV_FS_DEFAULT_DRIVE_LETTER 'A'
#endif
#define LV_USE_FS_POSIX 1
#if LV_USE_FS_POSIX
    #define LV_FS_POSIX_LETTER 'B'
    #define LV_FS_POSIX_BUFFER_SIZE 4096
#endif
#define LV_USE_FS_MEMFS 0
#define LV_USE_FS_LITTLEFS 0
#define LV_USE_FS_FATFS 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_UTHASH 0

/*====================
   FREE LAYOUT / SDKs
====================*/
#define LV_USE_OBJ_ID 0
#define LV_USE_OBJ_ID_BUILTIN 0
#define LV_USE_GRID 1
#define LV_USE_FLEX 1
#define LV_USE_OBJ_PROPERTY 0

/*====================
   LINUX DRIVERS
====================*/
#define LV_USE_LINUX_DRM        1
#define LV_USE_LINUX_FBDEV      1
/* Repaint the whole framebuffer every frame. In the default PARTIAL mode the
 * sys-layer mouse cursor leaves a smeared trail: its previous position isn't
 * reliably repainted on the tegra framebuffer. FULL mode redraws everything
 * each frame so the old cursor pixels are always overwritten. force_refresh
 * (set in createDisplayFbdev) is documented as intended for FULL/DIRECT. */
#define LV_LINUX_FBDEV_RENDER_MODE   LV_DISPLAY_RENDER_MODE_FULL
#define LV_USE_TFT_ESPI         0
#define LV_USE_EVDEV            1
#define LV_USE_LIBINPUT         0
#define LV_USE_WINDOWS          0
#define LV_USE_OPENGLES         0
#define LV_USE_QNX              0

/* Wayland client driver (src/drivers/wayland). Used when the firmware runs as
 * a Wayland client under a compositor (weston) instead of owning the DRM
 * device directly. Enabled by CMake via -DJETSON_DISPLAY_BACKEND=WAYLAND, which
 * passes LV_USE_WAYLAND=1 to the LVGL target. Default 0 keeps the wayland
 * sources compiled as no-ops in DRM/SDL builds. */
#ifndef LV_USE_WAYLAND
#define LV_USE_WAYLAND 0
#endif

/* SDL backend is pulled in only when JETSON_DISPLAY_BACKEND=SDL via CMake. */
#ifdef LV_USE_SDL
/* allow external override */
#endif

/*====================
   EXAMPLES / DEMO
====================*/
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0

/* clang-format on */

#endif /* LV_CONF_H */