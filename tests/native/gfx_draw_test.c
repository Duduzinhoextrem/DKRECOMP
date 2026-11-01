/* GPU integration: real sampled/indexed/instanced draws, distinct per-stage uniforms, blending/scissor,
 * RGBA8/float targets and negative viewports. Every captured pixel is checked; no game files are used. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../runtime/gfx/vk_draw.h"
#include "draw_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pixels(const char *path, int flipped) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    BITMAPFILEHEADER header;
    BITMAPINFOHEADER info;
    int ok = fread(&header, 1, sizeof header, file) == sizeof header && fread(&info, 1, sizeof info, file) == sizeof info;
    ok = ok && header.bfType == 0x4d42 && info.biWidth == 320 && info.biHeight == -240 && info.biBitCount == 32;
    if (ok) fseek(file, (long)header.bfOffBits, SEEK_SET);
    unsigned checked = 0;
    for (unsigned y = 0; ok && y < 240; y++) for (unsigned x = 0; ok && x < 320; x++) {
        int expected[4] = {191, 128, 64, 255}; /* BGRA clear */
        if (x >= 40 && x < 280 && y >= 60 && y < 180) {
            int left = x < 160, top = flipped ? y >= 120 : y < 120;
            int red = (top && left) || (!top && !left);
            int green = !left;
            int blue = !top;
            expected[0] = blue ? 255 : 0; expected[1] = green ? 64 : 0; expected[2] = red ? 128 : 0;
            if (x >= 100 && x < 220 && y >= 90 && y < 150) {
                expected[1] = green ? 160 : 0; expected[2] = red ? 192 : 0;
            }
        }
        uint8_t actual[4];
        if (fread(actual, 1, 4, file) != 4) { ok = 0; break; }
        for (unsigned c = 0; c < 4; c++) if (abs(actual[c] - expected[c]) > 2) {
            fprintf(stderr, "pixel (%u,%u) channel %u: got %u, expected %d (flipped=%d)\n", x, y, c, actual[c], expected[c], flipped);
            ok = 0; break;
        }
        checked++;
    }
    fclose(file);
    if (ok) printf("capture: %u pixels match draw/texture/uniform/blend/scissor, flipped=%d\n", checked, flipped);
    return ok ? 0 : -1;
}

#define TRY(call) do { if ((call) != 0) { fprintf(stderr, "%s: %s\n", #call, dkr_gfx_error(gfx)); failed = 1; goto done; } } while (0)
int main(int argc, char **argv) {
    int validation = -1, failed = 0;
    const char *capture = "gfx-draw";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--validation")) validation = 1;
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else return 2;
    }
    char paths[2][4096];
    if (snprintf(paths[0], sizeof paths[0], "%s-unorm.bmp", capture) >= (int)sizeof paths[0] ||
        snprintf(paths[1], sizeof paths[1], "%s-float.bmp", capture) >= (int)sizeof paths[1]) return 2;
    DkrGfx *gfx = NULL;
    DkrGfxConfig config = {320, 240, "DKRecomp synthetic GPU draw test", 0, validation};
    TRY(dkr_gfx_create(&gfx, &config));
    DkrVkImage *targets[2] = {NULL}, *texture = NULL;
    const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    for (unsigned i = 0; i < 2; i++) {
        DkrVkImageInfo info = {formats[i], 320, 240, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT};
        TRY(dkr_vk_image_create(gfx, &info, &targets[i]));
    }
    DkrVkImageInfo tex_info = {VK_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT};
    TRY(dkr_vk_image_create(gfx, &tex_info, &texture));
    const uint8_t texels[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    const float vertices[] = {99, 99, 99, 99, -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
    const uint16_t indices[] = {99, 99, 0, 1, 2, 2, 1, 3};
    DkrVkBuffer *vertex = NULL, *index = NULL;
    TRY(dkr_vk_buffer_create(gfx, vertices, sizeof vertices, &vertex));
    TRY(dkr_vk_buffer_create(gfx, indices, sizeof indices, &index));
    VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 1};
    DkrVkSampler *sampler = NULL;
    TRY(dkr_vk_sampler_create(gfx, &sampler_info, &sampler));
    const DkrVkShaderInfo shaders[] = {{0, draw_vert, sizeof draw_vert}, {1, draw_frag, sizeof draw_frag}};
    const DkrVkBinding bindings[] = {{0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {1, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {1, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    DkrVkProgram *program = NULL;
    TRY(dkr_vk_program_create(gfx, shaders, 2, bindings, 3, &program));
    const float clear[] = {.25f, .5f, .75f, 1}, transform[] = {.75f, .5f, 0, 0};
    const float color[] = {.5f, .25f, 1, 1}, overlay[] = {1, 1, 1, .5f};
    for (unsigned frame = 0; frame < 8; frame++) {
        unsigned kind = frame % 2;
        DkrVkBuffer *t = NULL, *c = NULL, *o = NULL;
        TRY(dkr_vk_batch_begin(gfx));
        if (!frame) TRY(dkr_vk_image_upload(gfx, texture, 0, 0, texels, sizeof texels));
        TRY(dkr_vk_image_clear(gfx, targets[kind], clear));
        TRY(dkr_vk_buffer_temporary(gfx, transform, sizeof transform, &t));
        TRY(dkr_vk_buffer_temporary(gfx, color, sizeof color, &c));
        TRY(dkr_vk_buffer_temporary(gfx, overlay, sizeof overlay, &o));
        DkrVkDescriptor descriptors[] = {{.stage = 0, .binding = 1, .buffer = t, .size = sizeof transform},
            {.stage = 1, .binding = 1, .buffer = c, .size = sizeof color},
            {.stage = 1, .binding = 2, .image = texture, .sampler = sampler}};
        DkrVkDraw draw = {0};
        draw.program = program; draw.colors[0] = targets[kind];
        draw.pipeline.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        draw.pipeline.attribute_count = 2; draw.pipeline.stream_count = 1; draw.pipeline.color_count = 1;
        draw.pipeline.attributes[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
        draw.pipeline.attributes[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32_SFLOAT, 8};
        draw.pipeline.streams[0] = (VkVertexInputBindingDescription){0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
        draw.pipeline.colors[0] = formats[kind]; draw.pipeline.blend[0].colorWriteMask = 15;
        draw.pipeline.depth_minus_w = kind; /* exercise the actual enabled depth-clip extension */
        draw.vertices[0] = vertex; draw.vertex_offsets[0] = 16;
        draw.viewport = (VkViewport){0, kind ? 240 : 0, 320, kind ? -240 : 240, 0, 1};
        draw.scissor = (VkRect2D){{0, 0}, {320, 240}};
        draw.descriptors = descriptors; draw.descriptor_count = 3;
        draw.count = 4; draw.instances = 2; draw.first_instance = 7;
        TRY(dkr_vk_draw(gfx, &draw));
        draw.pipeline.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        draw.pipeline.blend[0].blendEnable = VK_TRUE;
        draw.pipeline.blend[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        draw.pipeline.blend[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        draw.pipeline.blend[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        draw.pipeline.blend[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        draw.scissor = (VkRect2D){{100, 90}, {120, 60}};
        descriptors[1].buffer = o;
        draw.indices = index; draw.index_type = VK_INDEX_TYPE_UINT16; draw.index_offset = 2;
        draw.first = 1; draw.count = 6; draw.instances = 1;
        TRY(dkr_vk_draw(gfx, &draw));
        TRY(dkr_vk_batch_end(gfx));
        TRY(dkr_vk_image_present(gfx, targets[kind], frame >= 6 ? paths[kind] : NULL));
        if (!dkr_gfx_poll(gfx)) { failed = 1; goto done; }
    }
    if (dkr_gfx_frames(gfx) != 8 || pixels(paths[0], 0) || pixels(paths[1], 1)) failed = 1;
done:
    if (dkr_gfx_destroy(gfx)) failed = 1;
    printf("%s: GPU draw integration (8 frames, 2 captures, distinct VS/PS uniforms, texture, indexed/instanced, blend/scissor)\n", failed ? "FAIL" : "PASS");
    return failed;
}
