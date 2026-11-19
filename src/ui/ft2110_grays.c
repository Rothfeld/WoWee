/* Glue between the system FreeType's outlines and FreeType 2.1.10's gray
 * rasterizer (see include/ui/ft2110_grays.h). Compiled with _STANDALONE_ so
 * "ftimage.h" is the 2.1.10 one next to ftgrays.c, never the system's. */
/* As ftgrays.c defines them for a standalone build. */
#define FT_BEGIN_HEADER
#define FT_END_HEADER
#include "ftimage.h"
#include "ftgrays.h"

#include <stdlib.h>
#include <string.h>

#include "ui/ft2110_grays.h"

int wowee_ft2110_render(short n_contours, short n_points, const long* xy, const char* tags,
                        const short* contours, int even_odd, unsigned char* buffer, int width,
                        int rows) {
    /* FreeType 2.1.10's own render pool size (FT_RENDER_POOL_SIZE). */
    static long pool[16384 / sizeof(long)];
    FT_Vector* points = (FT_Vector*)malloc(sizeof(FT_Vector) * (size_t)(n_points > 0 ? n_points : 1));
    FT_Outline outline;
    FT_Bitmap target;
    FT_Raster_Params params;
    FT_Raster raster;
    int i, error;

    if (!points) return -4;
    for (i = 0; i < n_points; ++i) {
        points[i].x = xy[2 * i];
        points[i].y = xy[2 * i + 1];
    }
    memset(&outline, 0, sizeof outline);
    outline.n_contours = n_contours;
    outline.n_points = n_points;
    outline.points = points;
    outline.tags = (char*)tags;
    outline.contours = (short*)contours;
    outline.flags = even_odd ? FT_OUTLINE_EVEN_ODD_FILL : 0;

    memset(&target, 0, sizeof target);
    target.rows = rows;
    target.width = width;
    target.pitch = width;  /* positive pitch: row 0 is the top row */
    target.buffer = buffer;
    target.num_grays = 256;
    target.pixel_mode = FT_PIXEL_MODE_GRAY;

    memset(&params, 0, sizeof params);
    params.target = &target;
    params.source = &outline;
    params.flags = FT_RASTER_FLAG_AA;

    ft_grays_raster.raster_new(0, &raster);
    ft_grays_raster.raster_reset(raster, (char*)pool, (long)sizeof pool);
    error = ft_grays_raster.raster_render(raster, &params);
    ft_grays_raster.raster_done(raster);
    free(points);
    return error;
}
