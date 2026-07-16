#ifndef SOFT_RASTER_H
#define SOFT_RASTER_H

/*
 * A small software rasterizer for 32-bit framebuffers: anti-aliased
 * primitives, an embedded 8x16 bitmap font, sprite blits, and a letterbox
 * scaler.  Extracted from the software renderer shared by the
 * terminal-lander family of terminal games.
 *
 * The library is pure ISO C11 with no operating-system dependencies.  The
 * only allocation happens in sr_canvas_init(); every other call draws into
 * memory the canvas already owns, fully clipped to the canvas bounds.
 *
 * Pixel format
 * ------------
 * A canvas is a row-major array of uint32_t pixels laid out as 0xAARRGGBB:
 *
 * - Colors passed to drawing calls are 0x00RRGGBB; the high byte of a color
 *   argument is ignored.
 * - sr_clear() and sr_px() store the color with alpha 0xFF (opaque).
 * - sr_blend() and every primitive built on it move the RGB channels toward
 *   the requested color by the effective coverage and saturate the alpha
 *   byte toward 0xFF by the same coverage.  Coverage is quantized to 1/256
 *   steps with the same fixed-point math the games use, so results on an
 *   opaque canvas are byte-identical to the original renderers.
 * - On a canvas that starts fully transparent (sr_canvas_init() zeroes the
 *   pixels), drawing therefore builds a premultiplied-alpha sprite: RGB
 *   carries color scaled by coverage and the high byte carries coverage.
 *   sr_blit_alpha() and sr_blit_tint() composite such sprites over another
 *   canvas using that per-pixel alpha.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_VERSION_MAJOR 0
#define SR_VERSION_MINOR 1
#define SR_VERSION_PATCH 0

/* Embedded font glyph cell, before scaling. */
#define SR_FONT_W 8
#define SR_FONT_H 16

typedef struct sr_canvas {
    uint32_t *px;  /* row-major 0xAARRGGBB, px[y * w + x] */
    int w;
    int h;
    bool owns_px;  /* set by sr_canvas_init, cleared by sr_canvas_wrap */
} sr_canvas;

/*
 * Canvas lifetime
 * ---------------
 * sr_canvas_init() allocates w*h pixels, zeroed to transparent black
 * (0x00000000).  It fails (returns false, leaves *c zeroed) when w or h is
 * not positive, when the pixel count would overflow an int or the byte
 * count a size_t, or when allocation fails.
 *
 * sr_canvas_wrap() points the canvas at caller-owned memory of at least
 * w*h pixels without copying or clearing.  sr_canvas_free() releases memory
 * obtained by sr_canvas_init() and never frees wrapped memory; either way
 * it resets *c to an empty canvas.
 */
bool sr_canvas_init(sr_canvas *c, int w, int h);
void sr_canvas_wrap(sr_canvas *c, uint32_t *mem, int w, int h);
void sr_canvas_free(sr_canvas *c);

/* Color helpers: pack channels, linear mix by t in [0,1], multiply by k in
 * [0,2] with per-channel saturation. */
uint32_t sr_rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t sr_mix(uint32_t a, uint32_t b, float t);
uint32_t sr_scale_rgb(uint32_t rgb, float k);

/* Pixels.  sr_px() is a clipped opaque store; sr_blend() applies the
 * fixed-point coverage blend described above.  alpha outside [0,1] is
 * clamped. */
void sr_clear(sr_canvas *c, uint32_t rgb);
void sr_px(sr_canvas *c, int x, int y, uint32_t rgb);
void sr_blend(sr_canvas *c, int x, int y, uint32_t rgb, float alpha);

/*
 * Primitives.  Coordinates are floats measured in pixels; edges that fall
 * between pixel centers receive fractional-coverage anti-aliasing.
 *
 * - sr_fill_rect: axis-aligned rectangle with AA edges.
 * - sr_stroke_rect: rectangle outline built from four filled bars of the
 *   given line width.
 * - sr_fill_circle: filled disc with an anti-aliased rim.
 * - sr_ring: circle outline of the given stroke width, anti-aliased on
 *   both sides.
 * - sr_line: stroked segment of the given width with round caps, coverage
 *   computed from the distance to the segment.  dash_on/dash_off give the
 *   dash pattern in pixels along the line; pass 0, 0 for a solid line.
 * - sr_fill_triangle: filled triangle via edge functions (either winding).
 */
void sr_fill_rect(sr_canvas *c, float x, float y, float w, float h,
                  uint32_t rgb, float alpha);
void sr_stroke_rect(sr_canvas *c, float x, float y, float w, float h,
                    float line, uint32_t rgb, float alpha);
void sr_fill_circle(sr_canvas *c, float cx, float cy, float r,
                    uint32_t rgb, float alpha);
void sr_ring(sr_canvas *c, float cx, float cy, float r, float width,
             uint32_t rgb, float alpha);
void sr_line(sr_canvas *c, float x0, float y0, float x1, float y1,
             float width, uint32_t rgb, float alpha,
             int dash_on, int dash_off);
void sr_fill_triangle(sr_canvas *c, float x0, float y0, float x1, float y1,
                      float x2, float y2, uint32_t rgb, float alpha);

/*
 * Text over the embedded 8x16 font (ASCII 32..126; anything else renders
 * as '?').  scale is an integer pixel multiplier and is clamped to >= 1.
 * sr_text_width() returns the advance width of the string in pixels.
 * sr_text_outlined() draws a 1px black outline; sr_text_shadow() draws a
 * black drop shadow offset by one scaled pixel at 75% of alpha.
 */
int  sr_text_width(const char *s, int scale);
void sr_text(sr_canvas *c, float x, float y, const char *s,
             uint32_t rgb, float alpha, int scale);
void sr_text_center(sr_canvas *c, float cx, float y, const char *s,
                    uint32_t rgb, float alpha, int scale);
void sr_text_outlined(sr_canvas *c, float x, float y, const char *s,
                      uint32_t rgb, float alpha, int scale);
void sr_text_shadow(sr_canvas *c, float x, float y, const char *s,
                    uint32_t rgb, float alpha, int scale);

/*
 * Blits.  (x, y) is the destination of the source's top-left corner; every
 * blit clips against the destination bounds.
 *
 * - sr_blit: verbatim pixel copy, alpha byte included; source coverage is
 *   ignored.
 * - sr_blit_alpha: composites the source over the destination using each
 *   source pixel's alpha byte multiplied by the uniform alpha in [0,1].
 *   Source RGB is expected premultiplied by its alpha byte, which is what
 *   drawing into a transparent canvas produces; opaque pixels are plain
 *   0xFFRRGGBB.
 * - sr_blit_tint: like sr_blit_alpha but replaces the source color with
 *   rgb, using the source alpha purely as a mask.
 * - sr_blit_scaled: nearest-neighbor resample of the whole source into the
 *   w*h destination rectangle (source x = dst x * src_w / w), composited
 *   like sr_blit_alpha.
 * - sr_scale_canvas: scales the whole source onto the destination with
 *   nearest-neighbor sampling, preserving aspect ratio, centered, with
 *   opaque black letterbox bars; output alpha is forced opaque.
 */
void sr_blit(sr_canvas *dst, const sr_canvas *src, int x, int y);
void sr_blit_alpha(sr_canvas *dst, const sr_canvas *src, int x, int y,
                   float alpha);
void sr_blit_tint(sr_canvas *dst, const sr_canvas *src, int x, int y,
                  uint32_t rgb, float alpha);
void sr_blit_scaled(sr_canvas *dst, const sr_canvas *src, int x, int y,
                    int w, int h, float alpha);
void sr_scale_canvas(sr_canvas *dst, const sr_canvas *src);

/* Writes the canvas as a binary P6 PPM (alpha dropped).  Returns false on
 * an empty canvas or any I/O failure. */
bool sr_write_ppm(const sr_canvas *c, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SOFT_RASTER_H */
