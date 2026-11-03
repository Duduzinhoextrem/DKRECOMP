/* GPU writes captured vertex data back to host-visible buffers, with guard bytes around the selected range. */
#include "../../runtime/gfx/vk_draw.h"
#include "draw_shaders.h"
#include <stdio.h>
#include <string.h>
#define TRY(call) do { if ((call) != 0) { fprintf(stderr, "%s: %s\n", #call, dkr_gfx_error(gfx)); failed = 1; goto done; } } while (0)
int main(int argc, char **argv) {
    DkrGfx *gfx = NULL; int failed = 0;
    DkrGfxConfig config = {64, 64, "DKRecomp synthetic transform feedback", 0, argc > 1 && !strcmp(argv[1], "--validation") ? 1 : -1};
    TRY(dkr_gfx_create(&gfx, &config));
    if (!dkr_vk_feedback_supported(gfx)) { dkr_gfx_destroy(gfx); puts("SKIP: transform feedback unavailable"); return 77; }
    float positions[] = {1, 2, 3, 4, 5, 6};
    unsigned char initial[80]; memset(initial, 0xa5, sizeof initial);
    DkrVkBuffer *vertices = NULL, *feedback = NULL;
    TRY(dkr_vk_buffer_create(gfx, positions, sizeof positions, &vertices));
    TRY(dkr_vk_buffer_create(gfx, initial, sizeof initial, &feedback));
    DkrVkShaderInfo shader = {0, feedback_vert, sizeof feedback_vert};
    DkrVkProgram *program = NULL;
    TRY(dkr_vk_program_create(gfx, &shader, 1, NULL, 0, &program));
    TRY(dkr_vk_batch_begin(gfx));
    DkrVkDraw draw = {0}; draw.program = program;
    draw.pipeline.rasterizer_discard = 1; draw.pipeline.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    draw.pipeline.attribute_count = draw.pipeline.stream_count = 1;
    draw.pipeline.attributes[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    draw.pipeline.streams[0] = (VkVertexInputBindingDescription){0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
    draw.vertices[0] = vertices; draw.feedback[0] = feedback; draw.feedback_offsets[0] = 16; draw.feedback_sizes[0] = 48;
    draw.viewport = (VkViewport){0, 0, 1, 1, 0, 1}; draw.scissor = (VkRect2D){{0, 0}, {1, 1}};
    draw.count = 3; draw.instances = 1;
    TRY(dkr_vk_draw(gfx, &draw));
    TRY(dkr_vk_batch_end(gfx));
    unsigned char result[80];
    TRY(dkr_vk_buffer_read(gfx, feedback, 0, result, sizeof result));
    const float expected[] = {1, 2, 2, 3, 3, 4, 2, 3, 5, 6, 2, 3};
    if (memcmp(result + 16, expected, sizeof expected)) { fputs("FAIL: feedback vertices differ\n", stderr); failed = 1; }
    for (unsigned i = 0; i < 80; i++) if ((i < 16 || i >= 64) && result[i] != 0xa5) {
        fputs("FAIL: transform feedback modified a guard byte\n", stderr); failed = 1; break;
    }
done:
    if (dkr_gfx_destroy(gfx)) failed = 1;
    puts(failed ? "FAIL: GPU transform feedback" : "PASS: GPU transform feedback (12 floats and 32 guard bytes)");
    return failed;
}
