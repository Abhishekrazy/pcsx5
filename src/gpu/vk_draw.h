// Guest-GPU draw executor (Phase 5 M3.2c).
//
// Turns one AGC draw packet plus the Cx/Uc register shadow into real Vulkan
// work, keeping the 2D path only (no depth, linear tiling assumed):
//
//   * Guest image model: CB_COLOR0_BASE -> persistent VkImage + view +
//     framebuffer (render pass cached per attachment format).  Images are
//     seeded from guest memory on creation so the M1 CPU-side DMA clear is
//     visible; they live in COLOR_ATTACHMENT_OPTIMAL until M3.2d presents
//     from them.
//   * Textures: evaluated 8-dword image descriptors decoded via
//     gfx10_state, guest texels uploaded through a staging buffer
//     (linear), bound as combined image samplers.
//   * Pipeline: topology (VGT_PRIMITIVE_TYPE 0x242), blend
//     (CB_BLEND0_CONTROL + CB_TARGET_MASK), raster (PA_SU_SC_MODE_CNTL),
//     dynamic viewport/scissor/blend-constants.  Cached by (spirv digests
//     + state); shader modules cached by spirv digest.
//   * Descriptors: set 0 binding 0 is the StorageBuffer array of evaluated
//     buffer bindings (per-draw host-visible uploads) with the two
//     initial-scalar buffers appended (PS slot, then VS slot — the order
//     the draw path translates with); bindings 1..N are the textures in
//     global (vertex-then-pixel) order.  This is the binding contract of
//     gcn_translate.cpp.
//   * Index buffer: host-visible snapshot of index_count*(index_size?4:2)
//     bytes at index_addr; vkCmdDrawIndexed (vkCmdDraw for DrawIndexAuto).
//
// Recording strategy (M6): draws accumulate into one command buffer per
// flip and are submitted as a single batch when the presenter looks up the
// render target (VkDrawFlush).  The submit is not awaited — the present
// submit goes to the same queue afterwards, so queue order keeps the
// guarantee that the blit reads the image only after the draws finish.  The
// batch fence is waited only when per-batch resources are reused (once per
// flip on the steady path): per-draw host-visible uploads come from
// bump-allocated rings (staging / scalars / indices) sized for a batch, and
// replaced guest buffers are retired until the fence signals instead of
// being destroyed or overwritten in place.
#pragma once
#include "../common/types.h"
#include "vk_context.h"
#include <array>
#include <vector>

namespace GPU {

// One evaluated texture (global binding order: vertex images, then pixel).
struct VkDrawTexture {
    u64 guest_addr = 0;
    u32 width = 1, height = 1;
    u32 pitch = 0;            // texels per row, linear (0 = width)
    u32 data_format = 10, number_format = 0;
    u32 dst_select = 0xFAC;
    std::array<u32, 4> sampler{};
};

// One evaluated guest buffer range (scalar/buffer loads).
struct VkDrawBuffer {
    u64 guest_addr = 0;
    u64 size_bytes = 0;
};

struct VkDrawCall {
    // Translated SPIR-V (nullptr/0 when that stage is absent).
    const u32* vs_words = nullptr; size_t vs_word_count = 0;
    const u32* ps_words = nullptr; size_t ps_word_count = 0;

    // Binding-0 storage-buffer array contents, excluding the two scalar
    // slots the executor appends (PS scalars at index size(), VS at +1).
    std::vector<VkDrawBuffer> buffers;
    std::vector<u32> ps_scalars; // 256 dwords, GcnPackInitialScalarState
    std::vector<u32> vs_scalars;

    // Combined-sampler bindings 1..N in translator order.
    std::vector<VkDrawTexture> textures;

    // Fixed-function state (raw guest register values).
    u32 vgt_primitive_type = 4;
    u32 cb_blend0_control = 0;
    u32 cb_target_mask = 0xF;
    u32 pa_su_sc_mode_cntl = 0;
    std::array<u32, 4> blend_constants{}; // CB_BLEND_RED..ALPHA float bits

    // Viewport/scissor (raw register bits; see Gfx10::DecodeViewportScissor).
    u32 vport_xscale = 0, vport_xoffset = 0, vport_yscale = 0, vport_yoffset = 0;
    u32 screen_scissor_tl = 0, screen_scissor_br = 0;

    // Render target (CB_COLOR0_* decode input).
    u64 rt_base = 0;
    u32 rt_width = 0, rt_height = 0;
    u32 rt_format = 10, rt_number_type = 0;

    // Draw parameters.
    u64 index_addr = 0;
    u32 index_count = 0;   // also the vertex count for non-indexed draws
    u32 index_size = 0;    // 0 -> 16-bit indices, else 32-bit
    u32 instances = 1;
    bool indexed = false;
};

// Lifecycle: GPU::Initialize calls VkDrawInitialize when a VkContext exists;
// VkDrawExecute is a safe no-op (returns false) before that / after shutdown.
bool VkDrawInitialize(VkContext* ctx);
void VkDrawShutdown();
bool VkDrawIsReady();

// Executes one guest draw.  Returns false (with a log line, throttled per
// failure class) when the draw cannot be built — the frame simply misses it.
bool VkDrawExecute(const VkDrawCall& call);

// M6: submits the accumulated batch without waiting for it.  Called at the
// flip boundary (VkDrawLookupRenderTarget, which the backend invokes right
// before presenting) and at shutdown; also safe to call any time.
void VkDrawFlush();

// M3.2d: looks up the GPU image backing a guest render-target address.
// Returns false when no image exists for it (caller keeps the CPU upload
// present path).  Render-target images live in COLOR_ATTACHMENT_OPTIMAL
// between draws; the presenter must restore that layout after blitting.
bool VkDrawLookupRenderTarget(u64 guest_base, VkImage* image,
                              u32* width, u32* height);

} // namespace GPU
