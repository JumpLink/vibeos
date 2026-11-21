/* The Vulkan backend (see gfx-backend.h): one pipeline per GfxPipe with the instances as a per-instance vertex
 * buffer, a combined image sampler per texture, uploads through a per-frame staging buffer, on vk.c's device and
 * swapchain. */
#include "gfx-backend.h"
#include "vk.h"

#include <glib.h>
#include <string.h>

#include "quad_vert.h"
#include "quad_frag.h"
#include "blur_down_frag.h"
#include "blur_up_frag.h"
#include "stretch_frag.h"
#include "opaque_frag.h"

#define STAGING_BYTES (8u << 20)     /* per frame in flight; larger uploads go their own way */
#define MAX_SETS      2048

typedef struct {
  GfxTex          base;
  VkImage         image;
  VkDeviceMemory  mem;
  VkImageView     view;
  VkDescriptorSet set;
  VkFramebuffer   fb;         /* render targets only */
  VkFormat        format;
  VkImageLayout   layout;
  bool            swap_rb;    /* BGRA pixels into an RGBA image (an RGBA swapchain): swapped on the way in */
} VkTex;

static struct {
  Vk                    vk;
  VkDescriptorSetLayout dsl;
  VkDescriptorPool      pool;
  VkPipelineLayout      layout;
  VkPipeline            pipes[GFX_N_PIPES];
  VkSampler             sampler;
  VkShaderModule        vert;

  VkBuffer              inst_buf[VK_FRAMES];
  VkDeviceMemory        inst_mem[VK_FRAMES];
  GfxInst              *inst_map[VK_FRAMES];

  VkBuffer              stage_buf[VK_FRAMES];
  VkDeviceMemory        stage_mem[VK_FRAMES];
  guint8               *stage_map[VK_FRAMES];
  VkDeviceSize          stage_used;

  /* the frame being recorded */
  VkCommandBuffer       cmd;
  GfxPipe               bound_pipe;
  GfxPush               pushed;
  bool                  push_valid;
} V;

static VkShaderModule
shader (const uint32_t *code, size_t size)
{
  VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
  VkShaderModule m = VK_NULL_HANDLE;
  vkCreateShaderModule (V.vk.dev, &ci, NULL, &m);
  return m;
}

static VkPipeline
make_pipeline (const uint32_t *frag_code, size_t frag_size, bool blend)
{
  VkShaderModule frag = shader (frag_code, frag_size);
  VkPipelineShaderStageCreateInfo stages[2] = {
    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = V.vert, .pName = "main" },
    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = frag, .pName = "main" },
  };
  VkVertexInputBindingDescription bind = { 0, sizeof (GfxInst), VK_VERTEX_INPUT_RATE_INSTANCE };
  VkVertexInputAttributeDescription attrs[6];
  for (int i = 0; i < 6; i++)
    attrs[i] = (VkVertexInputAttributeDescription) { i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16 };
  VkPipelineVertexInputStateCreateInfo vi = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
    .vertexAttributeDescriptionCount = 6, .pVertexAttributeDescriptions = attrs,
  };
  VkPipelineInputAssemblyStateCreateInfo ia = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkPipelineViewportStateCreateInfo vp = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1,
  };
  VkPipelineRasterizationStateCreateInfo rs = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo ms = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineColorBlendAttachmentState cba = {
    .blendEnable = blend,
    .srcColorBlendFactor = VK_BLEND_FACTOR_ONE, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .colorBlendOp = VK_BLEND_OP_ADD,
    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .alphaBlendOp = VK_BLEND_OP_ADD,
    .colorWriteMask = 0xf,
  };
  VkPipelineColorBlendStateCreateInfo cb = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba,
  };
  VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo ds = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn,
  };
  VkGraphicsPipelineCreateInfo ci = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .stageCount = 2, .pStages = stages,
    .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
    .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
    .layout = V.layout,
    .renderPass = V.vk.rp_present,     /* the offscreen pass has the same format: compatible */
  };
  VkPipeline p = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines (V.vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS)
    g_warning ("gfx: pipeline creation failed");
  vkDestroyShaderModule (V.vk.dev, frag, NULL);
  return p;
}

/* everything init made, so that another backend can have the surface */
static void
teardown (void)
{
  Vk *vk = &V.vk;
  if (vk->dev) {
    vkDeviceWaitIdle (vk->dev);
    for (int i = 0; i < VK_FRAMES; i++) {
      vkDestroyBuffer (vk->dev, V.inst_buf[i], NULL);
      vkFreeMemory (vk->dev, V.inst_mem[i], NULL);
      vkDestroyBuffer (vk->dev, V.stage_buf[i], NULL);
      vkFreeMemory (vk->dev, V.stage_mem[i], NULL);
    }
    for (int i = 0; i < GFX_N_PIPES; i++)
      vkDestroyPipeline (vk->dev, V.pipes[i], NULL);
    vkDestroyShaderModule (vk->dev, V.vert, NULL);
    vkDestroySampler (vk->dev, V.sampler, NULL);
    vkDestroyPipelineLayout (vk->dev, V.layout, NULL);
    vkDestroyDescriptorPool (vk->dev, V.pool, NULL);
    vkDestroyDescriptorSetLayout (vk->dev, V.dsl, NULL);
  }
  vk_destroy (vk);
  memset (&V, 0, sizeof V);
}

static bool
setup (void)
{
  Vk *vk = &V.vk;
  VkDescriptorSetLayoutBinding b = {
    .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  VkDescriptorSetLayoutCreateInfo dli = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b,
  };
  VK_CHECK (vkCreateDescriptorSetLayout (vk->dev, &dli, NULL, &V.dsl));
  VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_SETS };
  VkDescriptorPoolCreateInfo dpi = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    .maxSets = MAX_SETS, .poolSizeCount = 1, .pPoolSizes = &ps,
  };
  VK_CHECK (vkCreateDescriptorPool (vk->dev, &dpi, NULL, &V.pool));
  VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof (GfxPush) };
  VkPipelineLayoutCreateInfo pli = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1, .pSetLayouts = &V.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
  };
  VK_CHECK (vkCreatePipelineLayout (vk->dev, &pli, NULL, &V.layout));
  VkSamplerCreateInfo sci = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .maxLod = 0,
  };
  VK_CHECK (vkCreateSampler (vk->dev, &sci, NULL, &V.sampler));

  V.vert = shader (quad_vert, sizeof quad_vert);
  V.pipes[GFX_PIPE_QUAD] = make_pipeline (quad_frag, sizeof quad_frag, true);
  V.pipes[GFX_PIPE_BLUR_DOWN] = make_pipeline (blur_down_frag, sizeof blur_down_frag, false);
  V.pipes[GFX_PIPE_BLUR_UP] = make_pipeline (blur_up_frag, sizeof blur_up_frag, false);
  V.pipes[GFX_PIPE_STRETCH] = make_pipeline (stretch_frag, sizeof stretch_frag, true);
  V.pipes[GFX_PIPE_OPAQUE] = make_pipeline (opaque_frag, sizeof opaque_frag, false);
  for (int i = 0; i < GFX_N_PIPES; i++)
    if (!V.pipes[i])
      return false;

  for (int i = 0; i < VK_FRAMES; i++) {
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!vk_alloc_buffer (vk, GFX_INST_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hv, &V.inst_buf[i], &V.inst_mem[i]) ||
        !vk_alloc_buffer (vk, STAGING_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hv, &V.stage_buf[i], &V.stage_mem[i]))
      return false;
    vkMapMemory (vk->dev, V.inst_mem[i], 0, GFX_INST_BYTES, 0, (void **) &V.inst_map[i]);
    vkMapMemory (vk->dev, V.stage_mem[i], 0, STAGING_BYTES, 0, (void **) &V.stage_map[i]);
  }
  return true;
}

static bool
vkb_init (struct wl_display *display, struct wl_surface *surface, int w, int h, bool allow_cpu)
{
  if (vk_init (&V.vk, display, surface, w, h, allow_cpu) && setup ())
    return true;
  teardown ();
  return false;
}

static void
vkb_resize (int w, int h)
{
  vk_resize (&V.vk, w, h);
}

static void
vkb_screen_size (int *w, int *h)
{
  *w = V.vk.extent.width;
  *h = V.vk.extent.height;
}

static void
vkb_finish (void)
{
  vkDeviceWaitIdle (V.vk.dev);
}

/* ---- textures ------------------------------------------------------------------------------------------------- */

static void vkb_tex_destroy (GfxTex *gt);

static GfxTex *
vkb_tex_new (int w, int h, GfxFormat gformat, bool target)
{
  Vk *vk = &V.vk;
  VkTex *t = g_new0 (VkTex, 1);
  t->base = (GfxTex) { w, h, gformat, target };
  /* the cairo order is BGRA in memory: BGRA images, but RGBA ones (swapped on upload) on an RGBA device */
  VkFormat format = target ? vk->format
                  : gformat == GFX_FORMAT_R8 ? VK_FORMAT_R8_UNORM
                  : vk->format == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM
                  : VK_FORMAT_B8G8R8A8_UNORM;
  t->swap_rb = !target && gformat == GFX_FORMAT_BGRA8 && format == VK_FORMAT_R8G8B8A8_UNORM;
  t->format = format;
  t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                            (target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  VkImageCreateInfo ici = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = { w, h, 1 },
    .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  if (vkCreateImage (vk->dev, &ici, NULL, &t->image) != VK_SUCCESS)
    goto fail;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements (vk->dev, t->image, &req);
  VkMemoryAllocateInfo mai = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
    .memoryTypeIndex = vk_memory_type (vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
  };
  if (vkAllocateMemory (vk->dev, &mai, NULL, &t->mem) != VK_SUCCESS)
    goto fail;
  vkBindImageMemory (vk->dev, t->image, t->mem, 0);
  VkComponentMapping sw = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
  VkImageViewCreateInfo vci = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = format, .components = sw, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
  };
  if (vkCreateImageView (vk->dev, &vci, NULL, &t->view) != VK_SUCCESS)
    goto fail;
  VkDescriptorSetAllocateInfo dai = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = V.pool, .descriptorSetCount = 1, .pSetLayouts = &V.dsl,
  };
  if (vkAllocateDescriptorSets (vk->dev, &dai, &t->set) != VK_SUCCESS)
    goto fail;
  VkDescriptorImageInfo dii = { V.sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  VkWriteDescriptorSet wds = {
    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->set, .descriptorCount = 1,
    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii,
  };
  vkUpdateDescriptorSets (vk->dev, 1, &wds, 0, NULL);
  if (target) {
    VkFramebufferCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = vk->rp_offscreen,
      .attachmentCount = 1, .pAttachments = &t->view, .width = w, .height = h, .layers = 1,
    };
    vkCreateFramebuffer (vk->dev, &fci, NULL, &t->fb);
  }
  return &t->base;
fail:
  vkb_tex_destroy (&t->base);
  return NULL;
}

static void
vkb_tex_destroy (GfxTex *gt)
{
  VkTex *t = (VkTex *) gt;
  Vk *vk = &V.vk;
  if (t->fb) vkDestroyFramebuffer (vk->dev, t->fb, NULL);
  if (t->set) vkFreeDescriptorSets (vk->dev, V.pool, 1, &t->set);
  if (t->view) vkDestroyImageView (vk->dev, t->view, NULL);
  if (t->image) vkDestroyImage (vk->dev, t->image, NULL);
  if (t->mem) vkFreeMemory (vk->dev, t->mem, NULL);
  g_free (t);
}

static void
barrier (VkCommandBuffer cmd, VkTex *t, VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access,
         VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
  VkImageMemoryBarrier b = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask = src_access, .dstAccessMask = dst_access,
    .oldLayout = t->layout, .newLayout = to,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = t->image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
  };
  vkCmdPipelineBarrier (cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
  t->layout = to;
}

/* A copy of tightly packed pixels from `buf` at `off` into t, recorded into cmd */
static void
record_copy (VkCommandBuffer cmd, VkTex *t, VkBuffer buf, VkDeviceSize off, int x, int y, int w, int h)
{
  barrier (cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy r = {
    .bufferOffset = off,
    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
    .imageOffset = { x, y, 0 }, .imageExtent = { w, h, 1 },
  };
  vkCmdCopyBufferToImage (cmd, buf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
  barrier (cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

/* rows of pixels into staging memory, packed, B and R swapped for an RGBA image */
static void
stage_rows (VkTex *t, guint8 *dst, const guint8 *src, int w, int h, int stride)
{
  int bpp = t->base.format == GFX_FORMAT_R8 ? 1 : 4;
  for (int y = 0; y < h; y++) {
    const guint8 *s = src + (gsize) y * stride;
    guint8 *d = dst + (gsize) y * w * bpp;
    if (!t->swap_rb) {
      memcpy (d, s, (gsize) w * bpp);
      continue;
    }
    for (int x = 0; x < w; x++, s += 4, d += 4) {
      d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
    }
  }
}

static bool
vkb_upload (GfxTex *gt, int x, int y, int w, int h, const void *pixels)
{
  VkTex *t = (VkTex *) gt;
  uint32_t slot = V.vk.frame_index % VK_FRAMES;
  int bpp = gt->format == GFX_FORMAT_R8 ? 1 : 4;
  gsize size = (gsize) w * h * bpp;
  VkDeviceSize off = (V.stage_used + 15) & ~(VkDeviceSize) 15;
  if (off + size > STAGING_BYTES)
    return false;
  stage_rows (t, V.stage_map[slot] + off, pixels, w, h, w * bpp);
  record_copy (V.cmd, t, V.stage_buf[slot], off, x, y, w, h);
  V.stage_used = off + size;
  return true;
}

static void
vkb_upload_now (GfxTex *gt, int x, int y, int w, int h, const void *pixels, int stride)
{
  VkTex *t = (VkTex *) gt;
  Vk *vk = &V.vk;
  int bpp = gt->format == GFX_FORMAT_R8 ? 1 : 4;
  gsize size = (gsize) w * h * bpp;
  VkBuffer buf;
  VkDeviceMemory mem;
  if (!vk_alloc_buffer (vk, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf, &mem))
    return;
  guint8 *map;
  vkMapMemory (vk->dev, mem, 0, size, 0, (void **) &map);
  stage_rows (t, map, pixels, w, h, stride);
  vkUnmapMemory (vk->dev, mem);
  VkCommandBuffer cmd = vk_oneshot_begin (vk);
  record_copy (cmd, t, buf, 0, x, y, w, h);
  vk_oneshot_end (vk, cmd);
  vkDestroyBuffer (vk->dev, buf, NULL);
  vkFreeMemory (vk->dev, mem, NULL);
}

/* ---- frames and passes ---------------------------------------------------------------------------------------- */

static GfxInst *
vkb_frame_begin (void)
{
  V.cmd = vk_frame_begin (&V.vk);
  if (!V.cmd)
    return NULL;
  V.stage_used = 0;
  V.push_valid = false;
  return V.inst_map[V.vk.frame_index % VK_FRAMES];
}

static void
vkb_frame_end (void)
{
  vk_frame_end (&V.vk);
  V.cmd = VK_NULL_HANDLE;
}

static void
vkb_pass_begin (GfxTex *gt, int w, int h, const float clear[4])
{
  VkTex *target = (VkTex *) gt;
  Vk *vk = &V.vk;
  VkClearValue cv = { .color = { .float32 = { clear[0], clear[1], clear[2], clear[3] } } };
  VkRenderPassBeginInfo bi = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
    .renderPass = target ? vk->rp_offscreen : vk->rp_present,
    .framebuffer = target ? target->fb : vk->fbs[vk->image_index],
    .renderArea = { { 0, 0 }, { w, h } },
    .clearValueCount = 1, .pClearValues = &cv,
  };
  vkCmdBeginRenderPass (V.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  if (target)
    target->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;   /* the pass's final layout */
  VkViewport vp = { 0, 0, w, h, 0, 1 };
  VkRect2D sc = { { 0, 0 }, { w, h } };
  vkCmdSetViewport (V.cmd, 0, 1, &vp);
  vkCmdSetScissor (V.cmd, 0, 1, &sc);
  uint32_t slot = vk->frame_index % VK_FRAMES;
  VkDeviceSize off = 0;
  vkCmdBindVertexBuffers (V.cmd, 0, 1, &V.inst_buf[slot], &off);
  V.bound_pipe = GFX_N_PIPES;
}

static void
vkb_pass_end (void)
{
  vkCmdEndRenderPass (V.cmd);
}

static void
vkb_draw (GfxPipe pipe, GfxTex *gt, const GfxPush *push, uint32_t first, uint32_t count)
{
  if (V.bound_pipe != pipe) {
    vkCmdBindPipeline (V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipes[pipe]);
    V.bound_pipe = pipe;
  }
  if (!V.push_valid || memcmp (&V.pushed, push, sizeof *push)) {
    vkCmdPushConstants (V.cmd, V.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                        sizeof *push, push);
    V.pushed = *push;
    V.push_valid = true;
  }
  vkCmdBindDescriptorSets (V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.layout, 0, 1, &((VkTex *) gt)->set, 0, NULL);
  vkCmdDraw (V.cmd, 6, count, 0, first);
}

const GfxBackend gfx_vk_backend = {
  .name = "vulkan",
  .frames_in_flight = VK_FRAMES,
  .max_upload = STAGING_BYTES / 2,
  .init = vkb_init,
  .resize = vkb_resize,
  .screen_size = vkb_screen_size,
  .finish = vkb_finish,
  .tex_new = vkb_tex_new,
  .tex_destroy = vkb_tex_destroy,
  .upload = vkb_upload,
  .upload_now = vkb_upload_now,
  .frame_begin = vkb_frame_begin,
  .frame_end = vkb_frame_end,
  .pass_begin = vkb_pass_begin,
  .pass_end = vkb_pass_end,
  .draw = vkb_draw,
};
