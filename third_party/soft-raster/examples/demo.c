/* Renders a composed scene into demo.ppm: gradient sky, anti-aliased
 * circles and rings, a dashed line, outlined text, a sprite built with
 * draw-into-canvas calls, then the whole frame letterbox-scaled into a
 * larger output canvas.  No terminal interaction. */
#include "soft_raster.h"

#include <stdio.h>
#include <stdlib.h>

static void build_moth_sprite(sr_canvas *spr)
{
    /* Drawing into a transparent canvas leaves per-pixel coverage in the
     * alpha byte, so the sprite composites cleanly over any background. */
    sr_fill_triangle(spr, 11.0f, 12.0f, 2.0f, 4.0f, 4.0f, 14.0f,
                     0xd9a441u, 1.0f);
    sr_fill_triangle(spr, 13.0f, 12.0f, 22.0f, 4.0f, 20.0f, 14.0f,
                     0xd9a441u, 1.0f);
    sr_fill_circle(spr, 12.0f, 12.0f, 3.2f, 0x8a5a2bu, 1.0f);
    sr_fill_circle(spr, 12.0f, 7.5f, 2.2f, 0xb2793cu, 1.0f);
    sr_line(spr, 10.5f, 6.0f, 8.5f, 2.5f, 1.0f, 0x8a5a2bu, 1.0f, 0, 0);
    sr_line(spr, 13.5f, 6.0f, 15.5f, 2.5f, 1.0f, 0x8a5a2bu, 1.0f, 0, 0);
    sr_px(spr, 11, 7, 0x241b1au);
    sr_px(spr, 13, 7, 0x241b1au);
}

int main(void)
{
    sr_canvas scene, sprite, output;

    if (!sr_canvas_init(&scene, 320, 180) ||
        !sr_canvas_init(&sprite, 24, 24) ||
        !sr_canvas_init(&output, 520, 240)) {
        (void)fprintf(stderr, "demo: canvas allocation failed\n");
        return EXIT_FAILURE;
    }

    /* Gradient sky, banded like a summer dusk. */
    for (int y = 0; y < scene.h; y++) {
        float t = (float)y / (float)scene.h;
        uint32_t band = sr_mix(0x1c2a52u, 0xd97b4au, t * t);
        sr_fill_rect(&scene, 0.0f, (float)y, (float)scene.w, 1.0f,
                     band, 1.0f);
    }

    /* Low sun with an anti-aliased rim and two halo rings. */
    sr_fill_circle(&scene, 244.0f, 118.0f, 26.0f, 0xffd166u, 1.0f);
    sr_ring(&scene, 244.0f, 118.0f, 34.0f, 2.0f, 0xffb347u, 0.55f);
    sr_ring(&scene, 244.0f, 118.0f, 44.0f, 1.5f, 0xff9a5cu, 0.30f);

    /* Distant ridge from triangles, then the ground plane. */
    sr_fill_triangle(&scene, -20.0f, 150.0f, 70.0f, 96.0f, 160.0f, 150.0f,
                     0x2c2438u, 1.0f);
    sr_fill_triangle(&scene, 90.0f, 150.0f, 200.0f, 82.0f, 320.0f, 150.0f,
                     0x241d30u, 1.0f);
    sr_fill_rect(&scene, 0.0f, 148.0f, 320.0f, 32.0f, 0x191426u, 1.0f);
    sr_line(&scene, 0.0f, 148.5f, 320.0f, 148.5f, 1.0f, 0x3d3355u,
            0.85f, 0, 0);

    /* Dashed flight path across the sky. */
    sr_line(&scene, 24.0f, 58.0f, 296.0f, 34.0f, 2.0f, 0xf8fafcu,
            0.65f, 6, 5);

    /* Moths: the same sprite blitted at several sizes, one tinted as a
     * far silhouette. */
    build_moth_sprite(&sprite);
    sr_blit_alpha(&scene, &sprite, 40, 44, 1.0f);
    sr_blit_scaled(&scene, &sprite, 130, 30, 36, 36, 0.9f);
    sr_blit_scaled(&scene, &sprite, 210, 60, 16, 16, 0.8f);
    sr_blit_tint(&scene, &sprite, 88, 70, 0x120e1cu, 0.85f);

    /* Framed caption block. */
    sr_fill_rect(&scene, 60.0f, 152.0f, 200.0f, 22.0f, 0x0c0a14u, 0.85f);
    sr_stroke_rect(&scene, 60.0f, 152.0f, 200.0f, 22.0f, 1.0f,
                   0xd97b4au, 0.9f);

    sr_text_outlined(&scene, 160.0f - (float)sr_text_width("SOFT-RASTER", 2) / 2.0f,
                     8.0f, "SOFT-RASTER", 0xffd166u, 1.0f, 2);
    sr_text_shadow(&scene, 160.0f - (float)sr_text_width("software rendering demo", 1) / 2.0f,
                   42.0f, "software rendering demo", 0xf8fafcu, 0.9f, 1);
    sr_text_center(&scene, 160.0f, 155.0f, "dusk over the ridge",
                   0xd9c8a0u, 1.0f, 1);

    /* Letterbox the 16:9 scene into a wider canvas and write it out. */
    sr_scale_canvas(&output, &scene);
    if (!sr_write_ppm(&output, "demo.ppm")) {
        (void)fprintf(stderr, "demo: could not write demo.ppm\n");
        sr_canvas_free(&scene);
        sr_canvas_free(&sprite);
        sr_canvas_free(&output);
        return EXIT_FAILURE;
    }

    (void)printf("wrote demo.ppm (%dx%d)\n", output.w, output.h);
    sr_canvas_free(&scene);
    sr_canvas_free(&sprite);
    sr_canvas_free(&output);
    return EXIT_SUCCESS;
}
