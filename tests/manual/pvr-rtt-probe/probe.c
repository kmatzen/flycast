/* Raw RTT evidence collector. Output is unclassified until its execution
 * environment has been independently recorded. No emulator-derived golden. */
#include <kos.h>
#include <dc/pvr.h>
#include <stdio.h>
#include <stdint.h>

#ifndef PVR_PROBE_PACK_MODE
#define PVR_PROBE_PACK_MODE 1
#endif
#ifndef PVR_PROBE_ALPHA
#define PVR_PROBE_ALPHA 255
#endif
#ifndef PVR_PROBE_ALPHA_THRESHOLD
#define PVR_PROBE_ALPHA_THRESHOLD 0
#endif
#ifndef PVR_PROBE_K
#define PVR_PROBE_K 0
#endif
#ifndef PVR_PROBE_DITHER
#define PVR_PROBE_DITHER 0
#endif
#if PVR_PROBE_DITHER != 0 && PVR_PROBE_DITHER != 1
#error "Probe dither must be zero or one"
#endif
#if PVR_PROBE_K < 0 || PVR_PROBE_K > 255
#error "Probe K must be a byte"
#endif
#if PVR_PROBE_ALPHA < 0 || PVR_PROBE_ALPHA > 255 || PVR_PROBE_ALPHA_THRESHOLD < 0 || PVR_PROBE_ALPHA_THRESHOLD > 255
#error "Probe alpha and threshold must be bytes"
#endif
#if PVR_PROBE_PACK_MODE < 0 || PVR_PROBE_PACK_MODE > 3
#error "RTT probe supports the four documented 16-bit framebuffer formats"
#endif

enum { WIDTH = 32, HEIGHT = 32, STRIDE = 36, GUARD = 32,
       TARGET_BYTES = STRIDE * HEIGHT * 2, ALLOCATION_BYTES = TARGET_BYTES + GUARD * 2 };

int main(void)
{
    dbgio_write_str("PVR_PROBE_STAGE init\n");
    if (pvr_init_defaults() < 0)
        return 1;
    dbgio_write_str("PVR_PROBE_STAGE allocate\n");
    pvr_ptr_t allocation = pvr_mem_malloc(ALLOCATION_BYTES);
    if (!allocation)
        return 2;
    volatile uint8_t *memory = (volatile uint8_t *)allocation;
    for (unsigned i = 0; i < ALLOCATION_BYTES; ++i)
        memory[i] = 0xa5;
    pvr_ptr_t target = (void *)((uintptr_t)allocation + GUARD);
    pvr_set_bg_color(0, 0, 0);
    dbgio_write_str("PVR_PROBE_STAGE initial_wait\n");
    if (pvr_wait_ready() < 0 || pvr_wait_render_done() < 0)
        return 3;

    /* Selected 16-bit format with explicit dither, K and alpha threshold.
     * pvr_render_start at the pinned SDK revision
     * does not replace FB_CFG_2; dump its final value to verify that assumption. */
    const uint32_t previous_pack = PVR_GET(PVR_FB_CFG_2);
    PVR_SET(PVR_FB_CFG_2, PVR_PROBE_PACK_MODE | (PVR_PROBE_DITHER << 3) | ((uint32_t)PVR_PROBE_K << 8)
            | ((uint32_t)PVR_PROBE_ALPHA_THRESHOLD << 16));
    dbgio_write_str("PVR_PROBE_STAGE begin_rtt\n");
    if (pvr_scene_begin_rtt(target, WIDTH, HEIGHT, STRIDE) < 0)
        return 4;
    pvr_poly_cxt_t context;
    pvr_poly_hdr_t header __attribute__((aligned(32)));
    pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
    /* Preserve vertex alpha without changing the opaque ONE/ZERO blend. */
    context.gen.alpha = true;
    pvr_poly_compile(&header, &context);
    if (pvr_list_begin(PVR_LIST_OP_POLY) < 0 || pvr_prim(&header, sizeof(header)) < 0)
        return 5;
    const float xy[4][2] = { {4, 28}, {4, 4}, {28, 28}, {28, 4} };
    for (unsigned i = 0; i < 4; ++i) {
        pvr_vertex_t vertex __attribute__((aligned(32))) = {0};
        vertex.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        vertex.x = xy[i][0];
        vertex.y = xy[i][1];
        vertex.z = 1;
        vertex.argb = ((uint32_t)PVR_PROBE_ALPHA << 24) | 0x00abcdef;
        if (pvr_prim(&vertex, sizeof(vertex)) < 0)
            return 6;
    }
    dbgio_write_str("PVR_PROBE_STAGE finish_rtt\n");
    if (pvr_list_finish() < 0 || pvr_scene_finish() < 0
            || pvr_wait_ready() < 0 || pvr_wait_render_done() < 0)
        return 7;

    dbgio_write_str("PVR_PROBE_STAGE readback\n");
    char line[192];
    snprintf(line, sizeof(line), "PVR_RTT_PROBE schema=5 execution=unclassified width=%u height=%u stride=%u guard=%u bytes=%u packmode=%u alpha=%02x threshold=%02x k=%02x dither=%u\n",
           WIDTH, HEIGHT, STRIDE, GUARD, ALLOCATION_BYTES, PVR_PROBE_PACK_MODE,
           PVR_PROBE_ALPHA, PVR_PROBE_ALPHA_THRESHOLD, PVR_PROBE_K, PVR_PROBE_DITHER);
    dbgio_write_str(line);
    snprintf(line, sizeof(line), "PVR_RTT_REG pack=%08lx base=%08lx stride=%08lx clipx=%08lx clipy=%08lx\n",
           (unsigned long)PVR_GET(PVR_FB_CFG_2), (unsigned long)PVR_GET(PVR_RENDER_ADDR),
           (unsigned long)PVR_GET(PVR_RENDER_MODULO), (unsigned long)PVR_GET(PVR_PCLIP_X),
           (unsigned long)PVR_GET(PVR_PCLIP_Y));
    dbgio_write_str(line);
    for (unsigned i = 0; i < ALLOCATION_BYTES; i += 32) {
        unsigned length = snprintf(line, sizeof(line), "PVR_RTT_BYTES offset=%04x data=", i);
        for (unsigned j = 0; j < 32 && i + j < ALLOCATION_BYTES; ++j)
            length += snprintf(line + length, sizeof(line) - length, "%02x", (unsigned)memory[i + j]);
        line[length++] = '\n';
        line[length] = '\0';
        dbgio_write_str(line);
    }
    dbgio_write_str("PVR_RTT_END\n");
    PVR_SET(PVR_FB_CFG_2, previous_pack);
    pvr_mem_free(allocation);
    pvr_shutdown();
    return 0;
}
