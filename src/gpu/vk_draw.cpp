// See vk_draw.h for the design notes.
#include "vk_draw.h"
#include "gfx10_state.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GPU {

namespace {

// FNV-1a over bytes — cache keys for modules/pipelines/samplers.
u64 HashBytes(u64 h, const void* data, size_t size) {
    const u8* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}
u64 HashU64(u64 h, u64 v) { return HashBytes(h, &v, sizeof(v)); }
constexpr u64 kHashSeed = 0xCBF29CE484222325ull;

struct HostBuffer {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize   size = 0;
};

struct TextureEntry {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
    u32            width = 0, height = 0;
    u32            layers = 1;     // > 1 for 2D-array uploads
    bool           arrayed = false; // view is VK_IMAGE_VIEW_TYPE_2D_ARRAY
    VkFormat       format = VK_FORMAT_UNDEFINED;
    u32            dst_select = 0;
    // Guest write-tracker generation of the last upload; -1 when the source
    // range is untracked (content treated as always stale — re-uploaded).
    s64            write_generation = -1;
};

struct RenderTargetEntry {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
    VkFramebuffer  fb = VK_NULL_HANDLE;
    u32            width = 0, height = 0;
    VkFormat       format = VK_FORMAT_UNDEFINED;
};

struct DrawState {
    VkContext*       ctx = nullptr;
    VkCommandPool    cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer  cmd = VK_NULL_HANDLE;
    VkFence          fence = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;

    // M6 batching: draws accumulate in `cmd` between flips; the batch is
    // submitted (not awaited) when the presenter looks up the render target.
    bool recording  = false; // cmd is open, not yet submitted
    bool in_flight  = false; // a batch was submitted; fence not yet consumed
    u32  draw_slot  = 0;     // draws recorded in the current batch

    HostBuffer   staging;     // texel/seed upload ring (host-visible)
    VkDeviceSize staging_off = 0;
    HostBuffer   scalar_ring; // kBatchDraws * 2 slots of 256 dwords
    HostBuffer   index_ring;  // bump-allocated per draw
    VkDeviceSize index_off = 0;

    // Host buffers replaced mid-batch; destroyed once the fence signals.
    std::vector<HostBuffer> retired;
    // Guest buffer bases already snapshotted by an earlier draw this batch
    // (a second upload of the same base gets a fresh buffer instead of
    // clobbering the snapshot the earlier draw will read).
    std::unordered_set<u64> uploaded_bases;

    std::unordered_map<u64, HostBuffer>     guest_buffers;  // base -> upload
    std::unordered_map<u64, TextureEntry>   textures;       // identity hash
    std::unordered_map<u64, RenderTargetEntry> render_targets; // guest base
    std::unordered_map<u64, VkRenderPass>   render_passes;  // format
    std::unordered_map<u64, VkSampler>      samplers;       // w0/w2 hash
    std::unordered_map<u64, VkShaderModule> modules;        // spirv digest
    std::unordered_map<u64, VkDescriptorSetLayout> set_layouts; // counts key
    std::unordered_map<u64, VkPipelineLayout>      pipe_layouts;
    std::unordered_map<u64, VkPipeline>     pipelines;      // state hash
};

// Per-batch limits.  Exceeding one rotates the batch (submit + one fence
// wait), so the wait is the resource-reuse sync point, not a per-draw one.
constexpr u32          kBatchDraws      = 64;
constexpr VkDeviceSize kScalarSlotBytes = 256 * sizeof(u32);
constexpr VkDeviceSize kIndexRingBytes  = 16ull * 1024 * 1024;
constexpr VkDeviceSize kStagingRingBytes = 64ull * 1024 * 1024;

DrawState  g_ds;
std::mutex g_draw_mutex;

void DestroyHostBuffer(HostBuffer& b) {
    VkContext* ctx = g_ds.ctx;
    if (b.buf) ctx->fn.DestroyBuffer(ctx->device, b.buf, nullptr);
    if (b.mem) ctx->fn.FreeMemory(ctx->device, b.mem, nullptr);
    b = HostBuffer{};
}

// (Re)creates a host-visible buffer of at least `size` bytes.
bool EnsureHostBuffer(HostBuffer& b, VkDeviceSize size, VkBufferUsageFlags usage) {
    if (b.buf && b.size >= size) return true;
    DestroyHostBuffer(b);
    VkContext* ctx = g_ds.ctx;

    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (ctx->fn.CreateBuffer(ctx->device, &bci, nullptr, &b.buf) != VK_SUCCESS) {
        LOG_WARN(GPU, "draw: buffer creation failed (%llu bytes)",
                 static_cast<unsigned long long>(size));
        return false;
    }
    VkMemoryRequirements req;
    ctx->fn.GetBufferMemoryRequirements(ctx->device, b.buf, &req);
    u32 type = 0;
    if (!VkFindMemoryType(ctx, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
        LOG_WARN(GPU, "draw: no host-visible memory type for buffer.");
        return false;
    }
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (ctx->fn.AllocateMemory(ctx->device, &mai, nullptr, &b.mem) != VK_SUCCESS ||
        ctx->fn.BindBufferMemory(ctx->device, b.buf, b.mem, 0) != VK_SUCCESS) {
        LOG_WARN(GPU, "draw: buffer memory allocation failed.");
        DestroyHostBuffer(b);
        return false;
    }
    b.size = size;
    return true;
}

bool WriteHostBuffer(const HostBuffer& b, VkDeviceSize offset,
                     const void* data, size_t size) {
    VkContext* ctx = g_ds.ctx;
    void* mapped = nullptr;
    if (ctx->fn.MapMemory(ctx->device, b.mem, offset, size, 0, &mapped) != VK_SUCCESS) {
        return false;
    }
    std::memcpy(mapped, data, size);
    ctx->fn.UnmapMemory(ctx->device, b.mem);
    return true;
}

// ---------------------------------------------------------------------------
// Batch lifecycle (M6).  Draws record into one command buffer; the batch is
// submitted at the flip (VkDrawFlush, called from VkDrawLookupRenderTarget)
// without waiting — the present submit goes to the same queue afterwards, so
// queue order guarantees the draws complete before the blit reads the image.
// The fence is waited exactly once per batch, when its host-visible rings
// (staging/scalars/indices), descriptor pool and retired buffers are reused.
// ---------------------------------------------------------------------------
bool BeginBatch() {
    VkContext* ctx = g_ds.ctx;
    if (g_ds.in_flight) {
        ctx->fn.WaitForFences(ctx->device, 1, &g_ds.fence, VK_TRUE, UINT64_MAX);
        ctx->fn.ResetFences(ctx->device, 1, &g_ds.fence);
        g_ds.in_flight = false;
        for (auto& b : g_ds.retired) DestroyHostBuffer(b);
        g_ds.retired.clear();
        ctx->fn.ResetDescriptorPool(ctx->device, g_ds.desc_pool, 0);
    }
    g_ds.draw_slot = 0;
    g_ds.staging_off = 0;
    g_ds.index_off = 0;
    g_ds.uploaded_bases.clear();
    ctx->fn.ResetCommandBuffer(g_ds.cmd, 0);

    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ctx->fn.BeginCommandBuffer(g_ds.cmd, &bi);
    g_ds.recording = true;
    return true;
}

void FlushBatch() {
    if (!g_ds.recording) return;
    VkContext* ctx = g_ds.ctx;
    ctx->fn.EndCommandBuffer(g_ds.cmd);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_ds.cmd;
    if (ctx->fn.QueueSubmit(ctx->queue, 1, &si, g_ds.fence) != VK_SUCCESS) {
        LOG_WARN(GPU, "draw: vkQueueSubmit failed.");
        g_ds.recording = false;
        return;
    }
    g_ds.recording = false;
    g_ds.in_flight = true;
}

// Ends the current batch and immediately opens a fresh one; used when a
// per-batch ring (draw slots / staging / indices / descriptor pool) is
// exhausted.  BeginBatch's fence wait is the buffer-reuse sync.
bool RotateBatch() {
    FlushBatch();
    return BeginBatch();
}

// Bump-allocates `size` bytes (256-aligned) from the staging ring for one
// upload in the current batch; rotates when full, grows the ring when a
// single upload exceeds its capacity (after the rotate, so the recorded
// copies referencing the old buffer have completed).
bool StagingAlloc(VkDeviceSize size, VkDeviceSize* offset) {
    if (!EnsureHostBuffer(g_ds.staging, kStagingRingBytes,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    if (size > g_ds.staging.size) {
        if (!RotateBatch()) return false;
        if (!EnsureHostBuffer(g_ds.staging, size,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
            return false;
        }
    }
    VkDeviceSize aligned = (g_ds.staging_off + 255) & ~static_cast<VkDeviceSize>(255);
    if (aligned + size > g_ds.staging.size) {
        if (!RotateBatch()) return false;
        aligned = 0;
    }
    g_ds.staging_off = aligned + size;
    *offset = aligned;
    return true;
}

// Same for the index ring (16-aligned; index offsets must be a multiple of
// the index size).
bool IndexAlloc(VkDeviceSize size, VkDeviceSize* offset) {
    if (!EnsureHostBuffer(g_ds.index_ring, kIndexRingBytes,
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
        return false;
    }
    if (size > g_ds.index_ring.size) {
        if (!RotateBatch()) return false;
        if (!EnsureHostBuffer(g_ds.index_ring, size,
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
            return false;
        }
    }
    VkDeviceSize aligned = (g_ds.index_off + 15) & ~static_cast<VkDeviceSize>(15);
    if (aligned + size > g_ds.index_ring.size) {
        if (!RotateBatch()) return false;
        aligned = 0;
    }
    g_ds.index_off = aligned + size;
    *offset = aligned;
    return true;
}

void ImageBarrier(VkImage image, VkImageLayout from, VkImageLayout to,
                  VkAccessFlags src_access, VkAccessFlags dst_access,
                  VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                  u32 layers = 1) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = layers;
    g_ds.ctx->fn.CmdPipelineBarrier(g_ds.cmd, src_stage, dst_stage, 0,
                                    0, nullptr, 0, nullptr, 1, &b);
}

bool CreateDeviceImage(VkImage& image, VkDeviceMemory& mem, u32 w, u32 h,
                       VkFormat format, VkImageUsageFlags usage,
                       u32 layers = 1) {
    VkContext* ctx = g_ds.ctx;
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (ctx->fn.CreateImage(ctx->device, &ici, nullptr, &image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    ctx->fn.GetImageMemoryRequirements(ctx->device, image, &req);
    u32 type = 0;
    if (!VkFindMemoryType(ctx, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        ctx->fn.DestroyImage(ctx->device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (ctx->fn.AllocateMemory(ctx->device, &mai, nullptr, &mem) != VK_SUCCESS ||
        ctx->fn.BindImageMemory(ctx->device, image, mem, 0) != VK_SUCCESS) {
        ctx->fn.DestroyImage(ctx->device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

VkImageView CreateImageView2D(VkImage image, VkFormat format, u32 dst_select,
                              bool arrayed = false, u32 layers = 1) {
    VkContext* ctx = g_ds.ctx;
    VkImageViewCreateInfo vci = {};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                           : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.components = Gfx10::DecodeComponentMapping(dst_select);
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = layers;
    VkImageView view = VK_NULL_HANDLE;
    if (ctx->fn.CreateImageView(ctx->device, &vci, nullptr, &view) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return view;
}

// Copies `bytes` from the staging ring at `staging_offset` into `image`
// (UNDEFINED/any -> SHADER_READ_ONLY or COLOR_ATTACHMENT_OPTIMAL) using the
// open command buffer.
void StageIntoImage(VkImage image, u32 w, u32 h, VkImageLayout final_layout,
                    VkDeviceSize staging_offset, u32 layers = 1) {
    ImageBarrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 layers);
    VkBufferImageCopy region = {};
    region.bufferOffset = staging_offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = layers;
    region.imageExtent = { w, h, 1 };
    g_ds.ctx->fn.CmdCopyBufferToImage(g_ds.cmd, g_ds.staging.buf, image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final_layout,
                 VK_ACCESS_TRANSFER_WRITE_BIT,
                 final_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                     ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                     : VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 final_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                     ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                     : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                 layers);
}

// Reads a guest memory range; false when unreadable.
bool ReadGuest(u64 addr, void* dst, size_t size) {
    if (!Memory::IsReadable(addr, size)) return false;
    Memory::ReadBuffer(addr, dst, size);
    return true;
}

// ---------------------------------------------------------------------------
// Textures.
// ---------------------------------------------------------------------------
u64 TextureIdentity(const VkDrawTexture& t, VkFormat format) {
    u64 h = kHashSeed;
    h = HashU64(h, t.guest_addr);
    h = HashU64(h, (static_cast<u64>(t.width) << 32) | t.height);
    h = HashU64(h, (static_cast<u64>(t.pitch) << 32) | t.dst_select);
    h = HashU64(h, static_cast<u64>(format));
    h = HashU64(h, (static_cast<u64>(t.tile_mode) << 32) |
                       ((static_cast<u64>(t.data_format) << 8) |
                        t.number_format));
    // mip_levels selects the mip-0 read offset inside the guest allocation.
    h = HashU64(h, t.mip_levels);
    // Array bindings upload/view a layered image (SharpEmu #471).
    h = HashU64(h, (static_cast<u64>(t.depth) << 1) |
                       (t.arrayed_view ? 1ull : 0ull));
    return h;
}

TextureEntry* EnsureTexture(const VkDrawTexture& t, VkFormat format) {
    const u64 key = TextureIdentity(t, format);
    auto it = g_ds.textures.find(key);
    if (it != g_ds.textures.end()) return &it->second;
    if (g_ds.textures.size() > 512) { // bound the cache; recreate everything
        // Recorded copies in the open batch may reference these images —
        // submit + wait first, then destroying them is safe.
        RotateBatch();
        for (auto& [k, e] : g_ds.textures) {
            g_ds.ctx->fn.DestroyImageView(g_ds.ctx->device, e.view, nullptr);
            g_ds.ctx->fn.DestroyImage(g_ds.ctx->device, e.image, nullptr);
            g_ds.ctx->fn.FreeMemory(g_ds.ctx->device, e.mem, nullptr);
        }
        g_ds.textures.clear();
    }

    TextureEntry e;
    // Arrayed bindings with real layers upload as one 2D-array image
    // (SharpEmu #471); single-layer arrayed bindings still get an array
    // view so the descriptor matches the shader's declared image type.
    e.layers = t.arrayed_view ? (t.depth > 1 ? t.depth : 1) : 1;
    e.arrayed = t.arrayed_view;
    if (!CreateDeviceImage(e.image, e.mem, t.width, t.height, format,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           e.layers)) {
        LOG_WARN(GPU, "draw: texture image creation failed (%ux%u fmt=%d).",
                 t.width, t.height, static_cast<int>(format));
        return nullptr;
    }
    e.view = CreateImageView2D(e.image, format, t.dst_select, e.arrayed,
                               e.layers);
    if (!e.view) {
        g_ds.ctx->fn.DestroyImage(g_ds.ctx->device, e.image, nullptr);
        g_ds.ctx->fn.FreeMemory(g_ds.ctx->device, e.mem, nullptr);
        return nullptr;
    }
    e.width = t.width; e.height = t.height;
    e.format = format; e.dst_select = t.dst_select;
    return &g_ds.textures.emplace(key, e).first->second;
}

// Uploads guest texels into the texture (staging ring, open cmd buffer).
// Linear surfaces keep the row-repack fast path; tiled surfaces
// (Gfx10::IsTiledSwizzleMode) are deswizzled first — at 4x4-block
// granularity for BC formats, which upload in their native VkFormat.
// Layered (2D-array) sources read every slice at the per-slice mip-chain
// stride and upload as one 2D-array image in a single copy region
// (SharpEmu #471).  The guest source range is write-tracked before the
// read so a CPU rewrite landing after the copy still bumps the write
// generation the cache skip compares against (SharpEmu #447);
// `generation_out` receives the generation captured before the read (-1
// when untracked) for the caller to record against the cache entry.
bool UploadTexture(TextureEntry& e, const VkDrawTexture& t,
                   const Gfx10::TexturePlan& plan, s64* generation_out) {
    // Element grid: texels, or 4x4 blocks for block-compressed formats.
    const u32 elem_w = plan.block_compressed ? (t.width + 3) / 4 : t.width;
    const u32 elem_h = plan.block_compressed ? (t.height + 3) / 4 : t.height;
    const u32 bpe = plan.bytes_per_element;
    if (bpe == 0) return false;
    const u32 layers = e.layers > 1 ? e.layers : 1;
    const size_t slice_bytes = static_cast<size_t>(elem_w) * elem_h * bpe;
    if (layers > 1 && slice_bytes > (256ull * 1024 * 1024) / layers) {
        return false;
    }

    std::vector<u8> pixels(slice_bytes * layers);
    if (t.tile_mode != 0) {
        // Tiled: the guest allocation is whole swizzle blocks, which can
        // far exceed the logical size — read the tiled span, then deswizzle.
        u64 tiled_bytes = 0;
        if (!Gfx10::IsTiledSwizzleMode(t.tile_mode) ||
            !Gfx10::TiledSurfaceByteCount(t.tile_mode, elem_w, elem_h, bpe,
                                          tiled_bytes) ||
            tiled_bytes == 0 || tiled_bytes > 256ull * 1024 * 1024) {
            LOG_WARN(GPU, "draw: unsupported texture swizzle mode %u "
                     "(%ux%u fmt=%d); draw dropped.", t.tile_mode, t.width,
                     t.height, static_cast<int>(plan.vk_format));
            return false;
        }
        // GFX10 mip chains are stored smallest-first: locate mip 0 within
        // the chain (byte offset, or element coords inside the tail block
        // when the whole chain fits there).  Single-level resources keep
        // reading from the descriptor base address.  Array slices are
        // spaced by the whole per-slice chain span.
        Gfx10::BaseMipPlacement mip0;
        const bool placed = t.mip_levels > 1 &&
            Gfx10::GetBaseMipPlacement(t.tile_mode, elem_w, elem_h, bpe,
                                       t.mip_levels, mip0);
        const u64 slice_stride = placed ? mip0.chain_slice_bytes : tiled_bytes;
        const u64 mip0_offset = placed ? mip0.byte_offset : 0;
        // Track before reading texels, then capture the generation: a CPU
        // rewrite landing after the copy bumps it and forces a refresh.
        Memory::TrackGuestWrites(
            t.guest_addr, slice_stride * (layers - 1) + mip0_offset + tiled_bytes);
        {
            u64 generation = 0;
            *generation_out = Memory::TryGetGuestWriteGeneration(
                t.guest_addr, &generation)
                    ? static_cast<s64>(generation) : -1;
        }
        std::vector<u8> tiled(static_cast<size_t>(tiled_bytes));
        for (u32 layer = 0; layer < layers; ++layer) {
            const u64 read_addr =
                t.guest_addr + static_cast<u64>(layer) * slice_stride + mip0_offset;
            if (!ReadGuest(read_addr, tiled.data(), tiled.size())) return false;
            u8* dst = pixels.data() + static_cast<size_t>(layer) * slice_bytes;
            const bool detiled = placed && mip0.in_mip_tail
                ? Gfx10::DetileTailMip0(tiled.data(), tiled.size(),
                                        dst, slice_bytes,
                                        t.tile_mode, elem_w, elem_h, bpe,
                                        mip0.tail_element_x, mip0.tail_element_y)
                : Gfx10::DetileSurface(tiled.data(), tiled.size(), dst,
                                       slice_bytes, t.tile_mode, elem_w, elem_h,
                                       bpe);
            if (!detiled) {
                return false;
            }
        }
    } else {
        // Linear fast path: tightly repack rows (guest pitch may exceed
        // width).  BC pitch counts texels; convert to blocks per row.
        const u32 pitch = t.pitch ? t.pitch : t.width;
        const u32 pitch_elems = plan.block_compressed ? (pitch + 3) / 4 : pitch;
        const VkDeviceSize need = static_cast<VkDeviceSize>(pitch_elems) *
                                  elem_h * bpe;
        if (need == 0 || need > 256ull * 1024 * 1024) return false;
        const VkDeviceSize row = static_cast<VkDeviceSize>(elem_w) * bpe;
        // Track before reading texels, then capture the generation (above).
        Memory::TrackGuestWrites(t.guest_addr, need * layers);
        {
            u64 generation = 0;
            *generation_out = Memory::TryGetGuestWriteGeneration(
                t.guest_addr, &generation)
                    ? static_cast<s64>(generation) : -1;
        }
        for (u32 layer = 0; layer < layers; ++layer) {
            const u64 slice_addr = t.guest_addr + need * layer;
            u8* dst = pixels.data() + static_cast<size_t>(layer) * slice_bytes;
            if (pitch_elems == elem_w) {
                if (!ReadGuest(slice_addr, dst, slice_bytes)) {
                    return false;
                }
            } else {
                for (u32 y = 0; y < elem_h; ++y) {
                    if (!ReadGuest(slice_addr +
                                   static_cast<u64>(y) * pitch_elems * bpe,
                                   dst + static_cast<size_t>(y) * row,
                                   static_cast<size_t>(row))) {
                        return false;
                    }
                }
            }
        }
    }
    VkDeviceSize off = 0;
    if (!StagingAlloc(static_cast<VkDeviceSize>(pixels.size()), &off)) return false;
    if (!WriteHostBuffer(g_ds.staging, off, pixels.data(), pixels.size())) return false;
    StageIntoImage(e.image, t.width, t.height, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   off, layers);
    return true;
}

// Shared opaque-white 1x1 texture for image pcs the scalar evaluation did
// not resolve (guest_addr == 0): the binding must still exist for the
// descriptor layout to match the translated module.  Arrayed bindings get
// a one-layer 2D-array view so the descriptor type still matches the
// shader's declared arrayed image (SharpEmu #471).
TextureEntry* EnsureFallbackTexture(bool arrayed) {
    const u64 key = arrayed ? ~0ull - 1 : ~0ull;
    auto it = g_ds.textures.find(key);
    if (it != g_ds.textures.end()) return &it->second;
    TextureEntry e;
    e.arrayed = arrayed;
    if (!CreateDeviceImage(e.image, e.mem, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return nullptr;
    }
    e.view = CreateImageView2D(e.image, VK_FORMAT_R8G8B8A8_UNORM, 0xFAC,
                               arrayed, 1);
    if (!e.view) {
        g_ds.ctx->fn.DestroyImage(g_ds.ctx->device, e.image, nullptr);
        g_ds.ctx->fn.FreeMemory(g_ds.ctx->device, e.mem, nullptr);
        return nullptr;
    }
    e.width = e.height = 1;
    e.format = VK_FORMAT_R8G8B8A8_UNORM;
    const u32 white = 0xFFFFFFFFu;
    VkDeviceSize off = 0;
    if (StagingAlloc(4, &off) && WriteHostBuffer(g_ds.staging, off, &white, 4)) {
        StageIntoImage(e.image, 1, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, off);
    }
    return &g_ds.textures.emplace(key, e).first->second;
}

VkSampler EnsureSampler(const std::array<u32, 4>& w) {    u64 h = kHashSeed;
    h = HashU64(h, w[0]);
    h = HashU64(h, w[2]);
    auto it = g_ds.samplers.find(h);
    if (it != g_ds.samplers.end()) return it->second;

    const Gfx10::SamplerState s = Gfx10::DecodeSampler(w.data());
    VkSamplerCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = s.mag;
    sci.minFilter = s.min;
    sci.mipmapMode = s.mip;
    sci.addressModeU = s.addr_x;
    sci.addressModeV = s.addr_y;
    sci.addressModeW = s.addr_z;
    sci.maxLod = 0.0f; // single mip level on the 2D path
    VkSampler sampler = VK_NULL_HANDLE;
    if (g_ds.ctx->fn.CreateSampler(g_ds.ctx->device, &sci, nullptr, &sampler) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    g_ds.samplers.emplace(h, sampler);
    return sampler;
}

// ---------------------------------------------------------------------------
// Render targets (guest image model).
// ---------------------------------------------------------------------------
VkRenderPass EnsureRenderPass(VkFormat format) {
    auto it = g_ds.render_passes.find(format);
    if (it != g_ds.render_passes.end()) return it->second;

    VkAttachmentDescription att = {};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // seeded at creation
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference ref = {};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkRenderPassCreateInfo rpci = {};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (g_ds.ctx->fn.CreateRenderPass(g_ds.ctx->device, &rpci, nullptr, &pass) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    g_ds.render_passes.emplace(format, pass);
    return pass;
}

RenderTargetEntry* EnsureRenderTarget(u64 base, u32 w, u32 h, VkFormat format) {
    auto it = g_ds.render_targets.find(base);
    if (it != g_ds.render_targets.end()) return &it->second;
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return nullptr;
    const u32 bpp = Gfx10::FormatBytesPerPixel(format);
    if (bpp == 0) return nullptr;

    RenderTargetEntry e;
    if (!CreateDeviceImage(e.image, e.mem, w, h, format,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT)) {
        LOG_WARN(GPU, "draw: render-target image creation failed (%ux%u fmt=%d).",
                 w, h, static_cast<int>(format));
        return nullptr;
    }
    e.view = CreateImageView2D(e.image, format, 0xFAC);
    if (!e.view) {
        g_ds.ctx->fn.DestroyImage(g_ds.ctx->device, e.image, nullptr);
        g_ds.ctx->fn.FreeMemory(g_ds.ctx->device, e.mem, nullptr);
        return nullptr;
    }
    e.width = w; e.height = h; e.format = format;

    // Seed from guest memory (PS5 render targets alias unified memory; this
    // is what makes the M1 CPU-side DMA clear visible on the GPU image).
    const VkDeviceSize need = static_cast<VkDeviceSize>(w) * h * bpp;
    VkDeviceSize off = 0;
    if (StagingAlloc(need, &off)) {
        std::vector<u8> zeros;
        const u8* src = nullptr;
        std::vector<u8> pixels;
        if (Memory::IsReadable(base, need)) {
            pixels.resize(static_cast<size_t>(need));
            Memory::ReadBuffer(base, pixels.data(), pixels.size());
            src = pixels.data();
        } else {
            zeros.assign(static_cast<size_t>(need), 0);
            src = zeros.data();
        }
        if (WriteHostBuffer(g_ds.staging, off, src, static_cast<size_t>(need))) {
            StageIntoImage(e.image, w, h, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, off);
        }
    }

    it = g_ds.render_targets.emplace(base, e).first;
    return &it->second;
}

bool EnsureFramebuffer(RenderTargetEntry& e, VkRenderPass pass) {
    if (e.fb) return true;
    VkFramebufferCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &e.view;
    fci.width = e.width;
    fci.height = e.height;
    fci.layers = 1;
    return g_ds.ctx->fn.CreateFramebuffer(g_ds.ctx->device, &fci, nullptr,
                                          &e.fb) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Shaders, layouts, pipelines.
// ---------------------------------------------------------------------------
u64 SpirvDigest(const u32* words, size_t count) {
    return HashBytes(kHashSeed, words, count * sizeof(u32));
}

VkShaderModule EnsureModule(const u32* words, size_t count, u64 digest) {
    auto it = g_ds.modules.find(digest);
    if (it != g_ds.modules.end()) return it->second;
    VkShaderModuleCreateInfo mci = {};
    mci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mci.codeSize = count * sizeof(u32);
    mci.pCode = words;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (g_ds.ctx->fn.CreateShaderModule(g_ds.ctx->device, &mci, nullptr, &mod) != VK_SUCCESS) {
        LOG_WARN(GPU, "draw: vkCreateShaderModule failed (%zu words).", count);
        return VK_NULL_HANDLE;
    }
    g_ds.modules.emplace(digest, mod);
    return mod;
}

u64 LayoutKey(u32 buffer_count, u32 image_count) {
    return (static_cast<u64>(buffer_count) << 32) | image_count;
}

VkPipelineLayout EnsurePipelineLayout(u32 buffer_count, u32 image_count,
                                      VkDescriptorSetLayout* set_layout_out) {
    const u64 key = LayoutKey(buffer_count, image_count);
    auto it = g_ds.pipe_layouts.find(key);
    if (it != g_ds.pipe_layouts.end()) {
        *set_layout_out = g_ds.set_layouts[key];
        return it->second;
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(1 + image_count);
    VkDescriptorSetLayoutBinding buffers = {};
    buffers.binding = 0;
    buffers.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    buffers.descriptorCount = buffer_count;
    buffers.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(buffers);
    for (u32 i = 0; i < image_count; ++i) {
        VkDescriptorSetLayoutBinding image = {};
        image.binding = 1 + i;
        image.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        image.descriptorCount = 1;
        image.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(image);
    }
    VkDescriptorSetLayoutCreateInfo lci = {};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = static_cast<u32>(bindings.size());
    lci.pBindings = bindings.data();
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (g_ds.ctx->fn.CreateDescriptorSetLayout(g_ds.ctx->device, &lci, nullptr,
                                               &set_layout) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    VkPipelineLayoutCreateInfo plci = {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (g_ds.ctx->fn.CreatePipelineLayout(g_ds.ctx->device, &plci, nullptr,
                                          &layout) != VK_SUCCESS) {
        g_ds.ctx->fn.DestroyDescriptorSetLayout(g_ds.ctx->device, set_layout, nullptr);
        return VK_NULL_HANDLE;
    }
    g_ds.set_layouts.emplace(key, set_layout);
    g_ds.pipe_layouts.emplace(key, layout);
    *set_layout_out = set_layout;
    return layout;
}

u64 PipelineKey(u64 vs_digest, u64 ps_digest, const VkDrawCall& call,
                u32 buffer_count, u32 image_count, VkFormat rt_format) {
    u64 h = kHashSeed;
    h = HashU64(h, vs_digest);
    h = HashU64(h, ps_digest);
    h = HashU64(h, LayoutKey(buffer_count, image_count));
    h = HashU64(h, call.vgt_primitive_type);
    h = HashU64(h, call.cb_blend0_control);
    h = HashU64(h, call.cb_target_mask & 0xFu);
    h = HashU64(h, call.pa_su_sc_mode_cntl & 0x7u);
    h = HashU64(h, static_cast<u64>(rt_format));
    return h;
}

VkPipeline EnsurePipeline(const VkDrawCall& call, u64 key, VkShaderModule vs,
                          VkShaderModule ps, VkPipelineLayout layout,
                          VkRenderPass pass) {
    auto it = g_ds.pipelines.find(key);
    if (it != g_ds.pipelines.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    u32 stage_count = 0;
    if (vs) {
        stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[stage_count].module = vs;
        stages[stage_count].pName = "main";
        ++stage_count;
    }
    if (ps) {
        stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[stage_count].module = ps;
        stages[stage_count].pName = "main";
        ++stage_count;
    }
    if (stage_count == 0) return VK_NULL_HANDLE;

    bool restart = false;
    const VkPrimitiveTopology topology =
        Gfx10::PrimitiveTopologyFromVgt(call.vgt_primitive_type, &restart);
    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = topology;
    ia.primitiveRestartEnable = restart ? VK_TRUE : VK_FALSE;

    // Guest shaders fetch vertex data through storage buffers; there is no
    // fixed-function vertex input on the Gen5 model.
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    const Gfx10::RasterState rs = Gfx10::DecodeRasterState(call.pa_su_sc_mode_cntl);
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = rs.cull_mode;
    raster.frontFace = rs.front_face;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // No depth on the 2D path.
    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    const Gfx10::BlendState bs =
        Gfx10::DecodeBlendState(call.cb_blend0_control, call.cb_target_mask);
    VkPipelineColorBlendAttachmentState att = {};
    att.blendEnable = bs.enable ? VK_TRUE : VK_FALSE;
    att.srcColorBlendFactor = bs.src_color;
    att.dstColorBlendFactor = bs.dst_color;
    att.colorBlendOp = bs.color_op;
    att.srcAlphaBlendFactor = bs.src_alpha;
    att.dstAlphaBlendFactor = bs.dst_alpha;
    att.alphaBlendOp = bs.alpha_op;
    att.colorWriteMask = bs.write_mask;
    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    const VkDynamicState dynamics[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 3;
    dyn.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo gpci = {};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = stage_count;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &raster;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = layout;
    gpci.renderPass = pass;
    gpci.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = g_ds.ctx->fn.CreateGraphicsPipelines(
        g_ds.ctx->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        LOG_WARN(GPU, "draw: vkCreateGraphicsPipelines failed (%d).", static_cast<int>(r));
        return VK_NULL_HANDLE;
    }
    g_ds.pipelines.emplace(key, pipeline);
    return pipeline;
}

// ---------------------------------------------------------------------------
// H6: compute pipeline layout + descriptor set layout.
// Binding model (same as graphics but with COMPUTE stage):
//   Set 0, Binding 0: StorageBuffer array  (buffer_count)
//   Set 0, Bindings 1..N: CombinedImageSampler or StorageImage (image_count)
// ---------------------------------------------------------------------------
VkPipelineLayout EnsureComputePipelineLayout(u32 buffer_count, u32 image_count,
                                              VkDescriptorSetLayout* set_layout_out,
                                              const std::vector<bool>& is_storage) {
    const u64 key = (static_cast<u64>(buffer_count) << 32) | image_count;
    const u64 ns_key = key | (1ull << 63); // separate namespace from graphics
    const auto pipe_it = g_ds.pipe_layouts.find(ns_key);
    if (pipe_it != g_ds.pipe_layouts.end()) {
        *set_layout_out = g_ds.set_layouts[key];
        return pipe_it->second;
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(std::max(1u, 1 + image_count));

    VkDescriptorSetLayoutBinding buffers = {};
    buffers.binding = 0;
    buffers.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    buffers.descriptorCount = buffer_count;
    buffers.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings.push_back(buffers);

    for (u32 i = 0; i < image_count; ++i) {
        VkDescriptorSetLayoutBinding image = {};
        image.binding = 1 + i;
        image.descriptorType = (i < is_storage.size() && is_storage[i])
            ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        image.descriptorCount = 1;
        image.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings.push_back(image);
    }

    VkDescriptorSetLayoutCreateInfo lci = {};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = static_cast<u32>(bindings.size());
    lci.pBindings = bindings.data();
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (g_ds.ctx->fn.CreateDescriptorSetLayout(g_ds.ctx->device, &lci, nullptr,
                                                &set_layout) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    VkPipelineLayoutCreateInfo plci = {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (g_ds.ctx->fn.CreatePipelineLayout(g_ds.ctx->device, &plci, nullptr,
                                           &layout) != VK_SUCCESS) {
        g_ds.ctx->fn.DestroyDescriptorSetLayout(g_ds.ctx->device, set_layout, nullptr);
        return VK_NULL_HANDLE;
    }
    g_ds.set_layouts.emplace(key, set_layout);
    g_ds.pipe_layouts.emplace(ns_key, layout);
    *set_layout_out = set_layout;
    return layout;
}

// Compute pipeline cache key (cs digest + layout key).
u64 ComputePipelineKey(u64 cs_digest, u32 buffer_count, u32 image_count) {
    u64 h = 0xB16B00B5C0FFEEull;
    h = HashU64(h, cs_digest);
    h = HashU64(h, (static_cast<u64>(buffer_count) << 32) | image_count);
    return h;
}

VkPipeline EnsureComputePipeline(VkShaderModule cs, const VkDispatchCall& call,
                                  VkPipelineLayout layout,
                                  u32 buffer_count, u32 image_count) {
    const u64 digest = SpirvDigest(call.cs_words, call.cs_word_count);
    const u64 key = ComputePipelineKey(digest, buffer_count, image_count);
    auto it = g_ds.pipelines.find(key);
    if (it != g_ds.pipelines.end()) return it->second;

    VkPipelineShaderStageCreateInfo stage = {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs;
    stage.pName = "main";

    VkComputePipelineCreateInfo cpci = {};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage = stage;
    cpci.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = g_ds.ctx->fn.CreateComputePipelines(
        g_ds.ctx->device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        LOG_WARN(GPU, "compute: vkCreateComputePipelines failed (%d).", static_cast<int>(r));
        return VK_NULL_HANDLE;
    }
    g_ds.pipelines.emplace(key, pipeline);
    LOG_INFO(GPU, "compute: created pipeline (cs=0x%llx, buffers=%u, images=%u)",
             digest, buffer_count, image_count);
    return pipeline;
}

// ---------------------------------------------------------------------------
// Guest buffers + descriptor set.
// ---------------------------------------------------------------------------
HostBuffer* EnsureGuestBuffer(u64 base, u64 size) {
    auto it = g_ds.guest_buffers.find(base);
    if (it != g_ds.guest_buffers.end() && it->second.size >= size) {
        return &it->second;
    }
    if (g_ds.guest_buffers.size() > 256) {
        // Retire rather than destroy: recorded batches may still reference
        // these buffers; they are freed once the batch fence signals.
        for (auto& [k, b] : g_ds.guest_buffers) g_ds.retired.push_back(b);
        g_ds.guest_buffers.clear();
        it = g_ds.guest_buffers.end();
    }
    HostBuffer b;
    if (!EnsureHostBuffer(b, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        return nullptr;
    }
    if (it != g_ds.guest_buffers.end()) g_ds.retired.push_back(it->second);
    return &g_ds.guest_buffers.insert_or_assign(base, b).first->second;
}

} // namespace

bool VkDrawInitialize(VkContext* ctx) {
    std::lock_guard<std::mutex> lk(g_draw_mutex);
    if (!ctx || !ctx->device) return false;
    g_ds.ctx = ctx;

    VkCommandPoolCreateInfo pci = {};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx->queue_family;
    if (ctx->fn.CreateCommandPool(ctx->device, &pci, nullptr, &g_ds.cmd_pool) != VK_SUCCESS) {
        LOG_WARN(GPU, "draw: command pool creation failed.");
        g_ds.ctx = nullptr;
        return false;
    }
    VkCommandBufferAllocateInfo cai = {};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_ds.cmd_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (ctx->fn.AllocateCommandBuffers(ctx->device, &cai, &g_ds.cmd) != VK_SUCCESS ||
        ctx->fn.CreateFence(ctx->device, &fci, nullptr, &g_ds.fence) != VK_SUCCESS) {
        LOG_WARN(GPU, "draw: command buffer / fence creation failed.");
        g_ds.ctx = nullptr;
        return false;
    }

    // Per-draw descriptor sets, recycled via vkResetDescriptorPool when the
    // batch fence is consumed (BeginBatch).
    const VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256 },  // H6: compute UAV images
    };
    VkDescriptorPoolCreateInfo dpci = {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 64;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    if (ctx->fn.CreateDescriptorPool(ctx->device, &dpci, nullptr,
                                     &g_ds.desc_pool) != VK_SUCCESS) {
        LOG_WARN(GPU, "draw: descriptor pool creation failed.");
        g_ds.ctx = nullptr;
        return false;
    }

    // Scalar-state ring: kBatchDraws slot pairs of 256 dwords each, one pair
    // per draw in the batch (PS slot at even offsets, VS at odd).
    if (!EnsureHostBuffer(g_ds.scalar_ring,
                          kBatchDraws * 2 * kScalarSlotBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        g_ds.ctx = nullptr;
        return false;
    }
    LOG_INFO(GPU, "Guest draw executor initialized (M6 batched).");
    return true;
}

bool VkDrawIsReady() {
    return g_ds.ctx != nullptr;
}

void VkDrawShutdown() {
    std::lock_guard<std::mutex> lk(g_draw_mutex);
    VkContext* ctx = g_ds.ctx;
    if (!ctx) return;
    FlushBatch(); // submit any recorded-but-unflushed draws before teardown
    ctx->fn.DeviceWaitIdle(ctx->device);
    for (auto& [k, p] : g_ds.pipelines) ctx->fn.DestroyPipeline(ctx->device, p, nullptr);
    for (auto& [k, l] : g_ds.pipe_layouts) ctx->fn.DestroyPipelineLayout(ctx->device, l, nullptr);
    for (auto& [k, l] : g_ds.set_layouts) ctx->fn.DestroyDescriptorSetLayout(ctx->device, l, nullptr);
    for (auto& [k, m] : g_ds.modules) ctx->fn.DestroyShaderModule(ctx->device, m, nullptr);
    for (auto& [k, s] : g_ds.samplers) ctx->fn.DestroySampler(ctx->device, s, nullptr);
    for (auto& [k, p] : g_ds.render_passes) ctx->fn.DestroyRenderPass(ctx->device, p, nullptr);
    for (auto& [k, e] : g_ds.textures) {
        ctx->fn.DestroyImageView(ctx->device, e.view, nullptr);
        ctx->fn.DestroyImage(ctx->device, e.image, nullptr);
        ctx->fn.FreeMemory(ctx->device, e.mem, nullptr);
    }
    for (auto& [k, e] : g_ds.render_targets) {
        ctx->fn.DestroyFramebuffer(ctx->device, e.fb, nullptr);
        ctx->fn.DestroyImageView(ctx->device, e.view, nullptr);
        ctx->fn.DestroyImage(ctx->device, e.image, nullptr);
        ctx->fn.FreeMemory(ctx->device, e.mem, nullptr);
    }
    for (auto& [k, b] : g_ds.guest_buffers) DestroyHostBuffer(b);
    for (auto& b : g_ds.retired) DestroyHostBuffer(b);
    DestroyHostBuffer(g_ds.scalar_ring);
    DestroyHostBuffer(g_ds.index_ring);
    DestroyHostBuffer(g_ds.staging);
    if (g_ds.desc_pool) ctx->fn.DestroyDescriptorPool(ctx->device, g_ds.desc_pool, nullptr);
    if (g_ds.fence) ctx->fn.DestroyFence(ctx->device, g_ds.fence, nullptr);
    if (g_ds.cmd_pool) ctx->fn.DestroyCommandPool(ctx->device, g_ds.cmd_pool, nullptr);
    g_ds = DrawState{};
}

void VkDrawFlush() {
    std::lock_guard<std::mutex> lk(g_draw_mutex);
    if (!g_ds.ctx) return;
    FlushBatch();
}

bool VkDrawLookupRenderTarget(u64 guest_base, VkImage* image,
                              u32* width, u32* height, VkFormat* format) {
    std::lock_guard<std::mutex> lk(g_draw_mutex);
    // Flip boundary: submit the pending batch (no wait — the present submit
    // goes to the same queue after this, so queue order keeps the M3.2d
    // guarantee that the blit reads the image only after the draws finish).
    if (g_ds.ctx) FlushBatch();
    auto it = g_ds.render_targets.find(guest_base);
    if (it == g_ds.render_targets.end()) return false;
    const RenderTargetEntry& e = it->second;
    if (image)  *image  = e.image;
    if (width)  *width  = e.width;
    if (height) *height = e.height;
    if (format) *format = e.format;
    return true;
}

bool VkDrawExecute(const VkDrawCall& call) {
    std::lock_guard<std::mutex> lk(g_draw_mutex);
    VkContext* ctx = g_ds.ctx;
    if (!ctx) return false;
    if (call.rt_base == 0 || call.rt_width == 0 || call.rt_height == 0) return false;
    if (call.index_count == 0) return false;

    const VkFormat rt_format =
        Gfx10::RenderTargetFormat(call.rt_format, call.rt_number_type);
    const u32 image_count = static_cast<u32>(call.textures.size());
    // Binding-0 array: evaluated buffers + PS scalar slot + VS scalar slot.
    const u32 buffer_count = static_cast<u32>(call.buffers.size()) + 2;

    const u64 vs_digest = call.vs_words ? SpirvDigest(call.vs_words, call.vs_word_count) : 0;
    const u64 ps_digest = call.ps_words ? SpirvDigest(call.ps_words, call.ps_word_count) : 0;

    // M6: accumulate draws into one command buffer per flip; the batch is
    // submitted (not awaited) by VkDrawFlush at present time.  The fence is
    // waited only when per-batch resources are reused (BeginBatch) — once
    // per flip on the steady path.
    if (!g_ds.recording) BeginBatch();
    if (g_ds.draw_slot >= kBatchDraws) RotateBatch();
    const u32 draw_slot = g_ds.draw_slot++;

    // Render target + render pass + framebuffer (guest image model).
    RenderTargetEntry* rt =
        EnsureRenderTarget(call.rt_base, call.rt_width, call.rt_height, rt_format);
    if (!rt) {
        return false;
    }
    VkRenderPass pass = EnsureRenderPass(rt->format);
    if (!pass || !EnsureFramebuffer(*rt, pass)) {
        return false;
    }

    // Textures: decode + upload + view (combined sampler bindings 1..N).
    std::vector<VkDescriptorImageInfo> image_infos(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        const VkDrawTexture& t = call.textures[i];
        TextureEntry* tex = nullptr;
        if (t.guest_addr == 0) {
            tex = EnsureFallbackTexture(t.arrayed_view);
        } else {
            const Gfx10::TexturePlan plan =
                Gfx10::PlanTextureFormat(t.data_format, t.number_format);
            if (!plan.supported) {
                LOG_WARN(GPU, "draw: unsupported texture format %u/%u; "
                         "fallback texture bound.", t.data_format,
                         t.number_format);
                tex = EnsureFallbackTexture(t.arrayed_view);
            } else {
                tex = EnsureTexture(t, plan.vk_format);
                if (tex) {
                    // Write-generation skip (SharpEmu #447): a guest CPU
                    // rewrite of the source memory bumps the tracked
                    // generation, which forces a fresh upload; an unchanged
                    // generation means the cached image is still current.
                    // Untracked sources keep the always-re-upload behavior.
                    u64 generation = 0;
                    const bool tracked = Memory::TryGetGuestWriteGeneration(
                        t.guest_addr, &generation);
                    const bool fresh = tracked && tex->write_generation >= 0 &&
                        static_cast<u64>(tex->write_generation) == generation;
                    if (!fresh) {
                        s64 uploaded_generation = -1;
                        if (!UploadTexture(*tex, t, plan,
                                           &uploaded_generation)) {
                            tex = nullptr;
                        } else {
                            tex->write_generation = uploaded_generation;
                        }
                    }
                }
            }
        }
        if (!tex) {
            return false;
        }
        VkSampler sampler = EnsureSampler(t.sampler);
        if (!sampler) {
            return false;
        }
        image_infos[i].sampler = sampler;
        image_infos[i].imageView = tex->view;
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // Storage buffers: per-draw host-visible uploads of the guest ranges.
    std::vector<VkDescriptorBufferInfo> buffer_infos(buffer_count);
    for (u32 i = 0; i < call.buffers.size(); ++i) {
        const VkDrawBuffer& b = call.buffers[i];
        u64 size = b.size_bytes ? b.size_bytes : 4;
        size = (size + 3) & ~3ull;
        if (size > 64ull * 1024 * 1024) size = 64ull * 1024 * 1024;
        HostBuffer* hb = nullptr;
        if (g_ds.uploaded_bases.count(b.guest_addr) != 0) {
            // An earlier draw in this batch already snapshotted this base;
            // re-uploading in place would clobber the data that draw reads.
            // Give this draw a fresh buffer and retire the old one at the
            // batch fence instead of waiting.
            HostBuffer fresh;
            if (!EnsureHostBuffer(fresh, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
                return false;
            }
            auto prev = g_ds.guest_buffers.find(b.guest_addr);
            if (prev != g_ds.guest_buffers.end()) g_ds.retired.push_back(prev->second);
            hb = &g_ds.guest_buffers.insert_or_assign(b.guest_addr, fresh).first->second;
        } else {
            hb = EnsureGuestBuffer(b.guest_addr, size);
        }
        if (!hb) {
            return false;
        }
        g_ds.uploaded_bases.insert(b.guest_addr);
        void* mapped = nullptr;
        if (ctx->fn.MapMemory(ctx->device, hb->mem, 0, size, 0, &mapped) != VK_SUCCESS ||
            !Memory::IsReadable(b.guest_addr, size)) {
            if (mapped) ctx->fn.UnmapMemory(ctx->device, hb->mem);
            return false;
        }
        Memory::ReadBuffer(b.guest_addr, mapped, static_cast<size_t>(size));
        ctx->fn.UnmapMemory(ctx->device, hb->mem);
        buffer_infos[i].buffer = hb->buf;
        buffer_infos[i].offset = 0;
        buffer_infos[i].range = size;
    }

    // Scalar-state slots (PS then VS — the translator's binding order), one
    // slot pair per draw from the scalar ring.
    const VkDeviceSize ps_off = draw_slot * 2 * kScalarSlotBytes;
    const VkDeviceSize vs_off = ps_off + kScalarSlotBytes;
    const VkDeviceSize scalar_offs[2] = { ps_off, vs_off };
    const std::vector<u32>* scalar_data[2] = { &call.ps_scalars, &call.vs_scalars };
    for (u32 s = 0; s < 2; ++s) {
        const u32 slot = static_cast<u32>(call.buffers.size()) + s;
        std::vector<u32> zeros(256, 0);
        const std::vector<u32>& data =
            scalar_data[s]->size() >= 256 ? *scalar_data[s] : zeros;
        if (!WriteHostBuffer(g_ds.scalar_ring, scalar_offs[s], data.data(),
                             kScalarSlotBytes)) {
            return false;
        }
        buffer_infos[slot].buffer = g_ds.scalar_ring.buf;
        buffer_infos[slot].offset = scalar_offs[s];
        buffer_infos[slot].range = kScalarSlotBytes;
    }

    // Index buffer snapshot (bump-allocated from the per-batch index ring).
    VkDeviceSize index_off = 0;
    if (call.indexed) {
        const u64 bytes = static_cast<u64>(call.index_count) *
                          (call.index_size != 0 ? 4 : 2);
        if (bytes == 0 || bytes > 64ull * 1024 * 1024 ||
            !IndexAlloc(bytes, &index_off)) {
            return false;
        }
        void* mapped = nullptr;
        if (ctx->fn.MapMemory(ctx->device, g_ds.index_ring.mem, index_off, bytes, 0,
                              &mapped) != VK_SUCCESS ||
            !Memory::IsReadable(call.index_addr, bytes)) {
            if (mapped) ctx->fn.UnmapMemory(ctx->device, g_ds.index_ring.mem);
            return false;
        }
        Memory::ReadBuffer(call.index_addr, mapped, static_cast<size_t>(bytes));
        ctx->fn.UnmapMemory(ctx->device, g_ds.index_ring.mem);
    }

    // Layout, modules, pipeline (cached).
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = EnsurePipelineLayout(buffer_count, image_count, &set_layout);
    if (!layout) {
        return false;
    }
    VkShaderModule vs = call.vs_words ? EnsureModule(call.vs_words, call.vs_word_count, vs_digest)
                                      : VK_NULL_HANDLE;
    VkShaderModule ps = call.ps_words ? EnsureModule(call.ps_words, call.ps_word_count, ps_digest)
                                      : VK_NULL_HANDLE;
    if ((call.vs_words && !vs) || (call.ps_words && !ps)) {
        return false;
    }
    const u64 pipe_key = PipelineKey(vs_digest, ps_digest, call, buffer_count,
                                     image_count, rt_format);
    VkPipeline pipeline = EnsurePipeline(call, pipe_key, vs, ps, layout, pass);
    if (!pipeline) {
        return false;
    }

    // Descriptor set (pool recycled when the batch fence is consumed).
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_ds.desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (ctx->fn.AllocateDescriptorSets(ctx->device, &dsai, &set) != VK_SUCCESS) {
        // Pool exhausted mid-batch: rotate (submit + wait + pool reset) and
        // retry the allocation once in the fresh batch.
        if (!RotateBatch()) return false;
        if (ctx->fn.AllocateDescriptorSets(ctx->device, &dsai, &set) != VK_SUCCESS) {
            return false;
        }
    }
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(1 + image_count);
    VkWriteDescriptorSet bw = {};
    bw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    bw.dstSet = set;
    bw.dstBinding = 0;
    bw.descriptorCount = buffer_count;
    bw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bw.pBufferInfo = buffer_infos.data();
    writes.push_back(bw);
    for (u32 i = 0; i < image_count; ++i) {
        VkWriteDescriptorSet iw = {};
        iw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        iw.dstSet = set;
        iw.dstBinding = 1 + i;
        iw.descriptorCount = 1;
        iw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        iw.pImageInfo = &image_infos[i];
        writes.push_back(iw);
    }
    ctx->fn.UpdateDescriptorSets(ctx->device, static_cast<u32>(writes.size()),
                                 writes.data(), 0, nullptr);

    // Record the draw.
    VkRenderPassBeginInfo rpb = {};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = pass;
    rpb.framebuffer = rt->fb;
    rpb.renderArea.offset = { 0, 0 };
    rpb.renderArea.extent = { rt->width, rt->height };
    ctx->fn.CmdBeginRenderPass(g_ds.cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    ctx->fn.CmdBindPipeline(g_ds.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    const Gfx10::ViewportScissor vps = Gfx10::DecodeViewportScissor(
        call.vport_xscale, call.vport_xoffset, call.vport_yscale, call.vport_yoffset,
        call.screen_scissor_tl, call.screen_scissor_br, rt->width, rt->height);
    ctx->fn.CmdSetViewport(g_ds.cmd, 0, 1, &vps.viewport);
    ctx->fn.CmdSetScissor(g_ds.cmd, 0, 1, &vps.scissor);
    float blend_constants[4];
    for (u32 i = 0; i < 4; ++i) {
        std::memcpy(&blend_constants[i], &call.blend_constants[i], 4);
    }
    ctx->fn.CmdSetBlendConstants(g_ds.cmd, blend_constants);
    ctx->fn.CmdBindDescriptorSets(g_ds.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  layout, 0, 1, &set, 0, nullptr);
    if (call.indexed) {
        ctx->fn.CmdBindIndexBuffer(g_ds.cmd, g_ds.index_ring.buf, index_off,
                                   call.index_size != 0 ? VK_INDEX_TYPE_UINT32
                                                        : VK_INDEX_TYPE_UINT16);
        ctx->fn.CmdDrawIndexed(g_ds.cmd, call.index_count, call.instances, 0, 0, 0);
    } else {
        ctx->fn.CmdDraw(g_ds.cmd, call.index_count, call.instances, 0, 0);
    }
    ctx->fn.CmdEndRenderPass(g_ds.cmd);
    // The batch stays open; VkDrawFlush (flip) submits it.
    return true;
}

// ---------------------------------------------------------------------------
// H6: compute dispatch executor (Phase 2+3).
// Supports buffer-only dispatches; texture/storage-image bindings are a
// future addition (H6.1 / H8) — the descriptor layout accepts them but
// EnsureTexture needs a VkFormat we do not carry yet.
// ---------------------------------------------------------------------------
bool VkDispatchExecute(const VkDispatchCall& call) {
    VkContext* ctx = g_ds.ctx;
    if (!ctx || !g_ds.recording) return false;
    if (!call.cs_words || call.cs_word_count == 0) return false;

    const u32 gx = call.group_count_x ? call.group_count_x : 1;
    const u32 gy = call.group_count_y ? call.group_count_y : 1;
    const u32 gz = call.group_count_z ? call.group_count_z : 1;
    const u32 image_count = static_cast<u32>(call.textures.size());

    // Flush any pending graphics batch — compute may consume its output.
    if (g_ds.draw_slot > 0) VkDrawFlush();

    // Upload buffers + initial scalar state into staging.
    VkDescriptorBufferInfo buffer_info = {};
    VkDeviceSize upload_off = 0;
    const u32 n_buf = static_cast<u32>(call.buffers.size());
    const bool has_buffers = n_buf > 0 || !call.cs_scalars.empty();
    if (has_buffers) {
        VkDeviceSize total = 0;
        for (const auto& b : call.buffers) total += b.size_bytes;
        total += kScalarSlotBytes;
        if (!StagingAlloc(total, &upload_off)) return false;
        u8* mapped = nullptr;
        if (ctx->fn.MapMemory(ctx->device, g_ds.staging.mem,
                              upload_off, total, 0,
                              reinterpret_cast<void**>(&mapped)) != VK_SUCCESS)
            return false;
        VkDeviceSize off = 0;
        for (const auto& b : call.buffers) {
            if (b.size_bytes > 0)
                Memory::ReadBuffer(b.guest_addr, mapped + off,
                                   static_cast<size_t>(b.size_bytes));
            off += b.size_bytes;
        }
        std::memcpy(mapped + off, call.cs_scalars.data(),
                    std::min(call.cs_scalars.size() * sizeof(u32),
                             static_cast<size_t>(kScalarSlotBytes)));
        ctx->fn.UnmapMemory(ctx->device, g_ds.staging.mem);
        buffer_info.buffer = g_ds.staging.buf;
        buffer_info.offset = upload_off;
        buffer_info.range = total;
    }

    // Image descriptors (H6.1 placeholder: textures not yet bound).
    std::vector<VkDescriptorImageInfo> image_infos(image_count);

    // Layout, module, pipeline (cached).
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = EnsureComputePipelineLayout(
        has_buffers ? 1u : 0u, image_count, &set_layout,
        call.texture_is_storage);
    if (!layout) return false;

    const u64 cs_digest = SpirvDigest(call.cs_words, call.cs_word_count);
    VkShaderModule cs = EnsureModule(call.cs_words, call.cs_word_count, cs_digest);
    if (!cs) return false;

    VkPipeline pipeline = EnsureComputePipeline(
        cs, call, layout, has_buffers ? 1u : 0u, image_count);
    if (!pipeline) return false;

    // Descriptor set.
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_ds.desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (ctx->fn.AllocateDescriptorSets(ctx->device, &dsai, &set) != VK_SUCCESS)
        return false;

    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(1 + image_count);
    if (has_buffers) {
        VkWriteDescriptorSet bw = {};
        bw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        bw.dstSet = set;
        bw.dstBinding = 0;
        bw.descriptorCount = 1;
        bw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bw.pBufferInfo = &buffer_info;
        writes.push_back(bw);
    }
    for (u32 i = 0; i < image_count; ++i) {
        VkWriteDescriptorSet iw = {};
        iw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        iw.dstSet = set;
        iw.dstBinding = 1 + i;
        iw.descriptorCount = 1;
        iw.descriptorType = (i < call.texture_is_storage.size() &&
                             call.texture_is_storage[i])
            ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        iw.pImageInfo = &image_infos[i];
        writes.push_back(iw);
    }
    ctx->fn.UpdateDescriptorSets(ctx->device, static_cast<u32>(writes.size()),
                                 writes.data(), 0, nullptr);

    // Record dispatch.
    ctx->fn.CmdBindPipeline(g_ds.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    ctx->fn.CmdBindDescriptorSets(g_ds.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  layout, 0, 1, &set, 0, nullptr);
    if (call.indirect && call.indirect_addr) {
        VkDispatchIndirectCommand cmd;
        Memory::ReadBuffer(call.indirect_addr, &cmd, sizeof(cmd));
        VkDeviceSize ioff = 0;
        if (StagingAlloc(sizeof(cmd), &ioff)) {
            u8* m = nullptr;
            if (ctx->fn.MapMemory(ctx->device, g_ds.staging.mem,
                                  ioff, sizeof(cmd), 0,
                                  reinterpret_cast<void**>(&m)) == VK_SUCCESS) {
                std::memcpy(m, &cmd, sizeof(cmd));
                ctx->fn.UnmapMemory(ctx->device, g_ds.staging.mem);
                ctx->fn.CmdDispatchIndirect(g_ds.cmd, g_ds.staging.buf, ioff);
            }
        }
    } else {
        ctx->fn.CmdDispatch(g_ds.cmd, gx, gy, gz);
    }
    return true;
}

} // namespace GPU
