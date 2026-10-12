/* Minimal changing-frame workload, not a PVR conformance golden. */
#include <kos.h>
#include <dc/pvr.h>
#include <stdio.h>

int main(void)
{
    if (pvr_init_defaults() < 0)
        return 1;
    PVR_SET(PVR_FB_CFG_2, 1); /* RGB565, no dithering. */
    pvr_set_bg_color(0, 0, 0);
    pvr_poly_cxt_t context;
    pvr_poly_hdr_t header __attribute__((aligned(32)));
    pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
    pvr_poly_compile(&header, &context);
    dbgio_write_str("PVR_CADENCE_PROBE schema=1 execution=unclassified warmup=60 measured=600 width=640 height=480\n");
    unsigned start_vblank = 0;
    char line[160];
    for (unsigned frame = 0; frame < 660; ++frame) {
        if (pvr_wait_ready() < 0)
            return 2;
        if (frame == 60)
            start_vblank = pvr_get_vbl_count();
        pvr_scene_begin();
        if (pvr_list_begin(PVR_LIST_OP_POLY) < 0 || pvr_prim(&header, sizeof(header)) < 0)
            return 3;
        /* Every frame remains distinct after RGB565 packing. */
        const uint32_t color = 0xff000080u | ((frame & 31u) << 19) | (((frame >> 5) & 63u) << 10);
        const float xy[4][2] = {{0, 480}, {0, 0}, {640, 480}, {640, 0}};
        for (unsigned i = 0; i < 4; ++i) {
            pvr_vertex_t vertex __attribute__((aligned(32))) = {0};
            vertex.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            vertex.x = xy[i][0];
            vertex.y = xy[i][1];
            vertex.z = 1;
            vertex.argb = color;
            if (pvr_prim(&vertex, sizeof(vertex)) < 0)
                return 4;
        }
        if (pvr_list_finish() < 0 || pvr_scene_finish() < 0)
            return 5;
        if (frame >= 60 && (frame + 1 - 60) % 120 == 0) {
            snprintf(line, sizeof(line), "PVR_CADENCE_PROGRESS submitted=%u vblanks=%u\n",
                     frame + 1 - 60, (unsigned)pvr_get_vbl_count() - start_vblank);
            dbgio_write_str(line);
        }
    }
    if (pvr_wait_ready() < 0 || pvr_wait_render_done() < 0)
        return 6;
    vid_waitvbl();
    vid_waitvbl();
    dbgio_write_str("PVR_CADENCE_END submitted=660\n");
    /* Retain the last image until the external harness stops the emulator. */
    for (;;)
        thd_sleep(1000);
}
