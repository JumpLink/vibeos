/* The Vulkan device and swapchain (see vk.h). */
#define VK_USE_PLATFORM_WAYLAND_KHR
#include "vk.h"

#include <glib.h>
#include <string.h>
#include <vulkan/vulkan_wayland.h>

static bool
pick_device (Vk *vk)
{
  uint32_t n = 0;
  vkEnumeratePhysicalDevices (vk->inst, &n, NULL);
  if (!n) {
    g_warning ("vulkan: no device (panvk on Bifrost needs PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1)");
    return false;
  }
  VkPhysicalDevice devs[8];
  n = MIN (n, 8);
  vkEnumeratePhysicalDevices (vk->inst, &n, devs);
  /* a GPU first: with every installed ICD enumerated (no VK_DRIVER_FILES), the software rasterizer (lavapipe) can
   * come before the phone's GPU; it is only taken when nothing else can present */
  for (int cpu = 0; cpu < 2; cpu++) {
    for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties (devs[i], &props);
      if ((props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) != cpu)
        continue;
      uint32_t nq = 0;
      vkGetPhysicalDeviceQueueFamilyProperties (devs[i], &nq, NULL);
      VkQueueFamilyProperties qp[16];
      nq = MIN (nq, 16);
      vkGetPhysicalDeviceQueueFamilyProperties (devs[i], &nq, qp);
      for (uint32_t q = 0; q < nq; q++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR (devs[i], q, vk->surface, &present);
        if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
          vk->phys = devs[i];
          vk->qfam = q;
          vk->props = props;
          vkGetPhysicalDeviceMemoryProperties (devs[i], &vk->memp);
          g_message ("vulkan: %s (%s), api %u.%u", props.deviceName,
                     props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                     : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "cpu" : "other",
                     VK_VERSION_MAJOR (props.apiVersion), VK_VERSION_MINOR (props.apiVersion));
          return true;
        }
      }
    }
  }
  g_warning ("vulkan: no device can present to the surface");
  return false;
}

static VkRenderPass
make_render_pass (Vk *vk, VkImageLayout final_layout)
{
  VkAttachmentDescription att = {
    .format = vk->format,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .finalLayout = final_layout,
  };
  VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
  VkSubpassDescription sub = {
    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
    .colorAttachmentCount = 1,
    .pColorAttachments = &ref,
  };
  VkSubpassDependency deps[2] = {
    { /* earlier passes' writes (offscreen targets) and the acquire before we write */
      .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
    },
    { /* what we wrote, for a later pass that samples it */
      .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    },
  };
  VkRenderPassCreateInfo ci = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
    .attachmentCount = 1, .pAttachments = &att,
    .subpassCount = 1, .pSubpasses = &sub,
    .dependencyCount = 2, .pDependencies = deps,
  };
  VkRenderPass rp = VK_NULL_HANDLE;
  vkCreateRenderPass (vk->dev, &ci, NULL, &rp);
  return rp;
}

static void
destroy_swapchain_images (Vk *vk)
{
  for (uint32_t i = 0; i < vk->nimages; i++) {
    if (vk->fbs[i]) vkDestroyFramebuffer (vk->dev, vk->fbs[i], NULL);
    if (vk->views[i]) vkDestroyImageView (vk->dev, vk->views[i], NULL);
    if (vk->rendered[i]) vkDestroySemaphore (vk->dev, vk->rendered[i], NULL);
    vk->fbs[i] = VK_NULL_HANDLE;
    vk->views[i] = VK_NULL_HANDLE;
    vk->rendered[i] = VK_NULL_HANDLE;
  }
}

static bool
make_swapchain (Vk *vk, uint32_t w, uint32_t h)
{
  VkSurfaceCapabilitiesKHR caps;
  VK_CHECK (vkGetPhysicalDeviceSurfaceCapabilitiesKHR (vk->phys, vk->surface, &caps));

  /* On Wayland the surface has no size of its own (currentExtent is 0xFFFFFFFF): it is what we make it */
  VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D) { w, h };
  ext.width = CLAMP (ext.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  ext.height = CLAMP (ext.height, caps.minImageExtent.height, caps.maxImageExtent.height);

  uint32_t nm = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR (vk->phys, vk->surface, &nm, NULL);
  VkPresentModeKHR modes[8];
  nm = MIN (nm, 8);
  vkGetPhysicalDeviceSurfacePresentModesKHR (vk->phys, vk->surface, &nm, modes);
  /* Mailbox never blocks the main loop in acquire; frames are paced by the compositor's frame callbacks instead */
  vk->present_mode = VK_PRESENT_MODE_FIFO_KHR;
  for (uint32_t i = 0; i < nm; i++)
    if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
      vk->present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
  if (g_getenv ("NEO_FIFO"))
    vk->present_mode = VK_PRESENT_MODE_FIFO_KHR;

  uint32_t count = MAX (caps.minImageCount, 3);
  if (caps.maxImageCount)
    count = MIN (count, caps.maxImageCount);

  VkCompositeAlphaFlagBitsKHR alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                        : VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
  VkSwapchainKHR old = vk->swapchain;
  VkSwapchainCreateInfoKHR ci = {
    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
    .surface = vk->surface,
    .minImageCount = count,
    .imageFormat = vk->format,
    .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
    .imageExtent = ext,
    .imageArrayLayers = 1,
    .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
    .compositeAlpha = alpha,
    .presentMode = vk->present_mode,
    .clipped = VK_TRUE,
    .oldSwapchain = old,
  };
  VK_CHECK (vkCreateSwapchainKHR (vk->dev, &ci, NULL, &vk->swapchain));
  if (old) {
    destroy_swapchain_images (vk);
    vkDestroySwapchainKHR (vk->dev, old, NULL);
  }
  vk->extent = ext;

  vk->nimages = VK_MAX_IMAGES;
  VK_CHECK (vkGetSwapchainImagesKHR (vk->dev, vk->swapchain, &vk->nimages, vk->images));
  for (uint32_t i = 0; i < vk->nimages; i++) {
    VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = vk->images[i],
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = vk->format,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VK_CHECK (vkCreateImageView (vk->dev, &vci, NULL, &vk->views[i]));
    VkFramebufferCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = vk->rp_present,
      .attachmentCount = 1, .pAttachments = &vk->views[i],
      .width = ext.width, .height = ext.height, .layers = 1,
    };
    VK_CHECK (vkCreateFramebuffer (vk->dev, &fci, NULL, &vk->fbs[i]));
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_CHECK (vkCreateSemaphore (vk->dev, &sci, NULL, &vk->rendered[i]));
  }
  g_message ("vulkan: swapchain %ux%u, %u images, %s", ext.width, ext.height, vk->nimages,
             vk->present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
  vk->recreate = false;
  return true;
}

bool
vk_init (Vk *vk, struct wl_display *display, struct wl_surface *surface, uint32_t w, uint32_t h)
{
  memset (vk, 0, sizeof *vk);
  const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME };
  VkApplicationInfo app = {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "neo-launcher",
    .applicationVersion = 3,
    .pEngineName = "neo",
    .apiVersion = VK_API_VERSION_1_0,
  };
  const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
  VkInstanceCreateInfo ici = {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &app,
    .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts,
    .enabledLayerCount = g_getenv ("NEO_VALIDATE") ? 1 : 0, .ppEnabledLayerNames = layers,
  };
  VK_CHECK (vkCreateInstance (&ici, NULL, &vk->inst));

  VkWaylandSurfaceCreateInfoKHR sci = {
    .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
    .display = display, .surface = surface,
  };
  VK_CHECK (vkCreateWaylandSurfaceKHR (vk->inst, &sci, NULL, &vk->surface));
  if (!pick_device (vk))
    return false;

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = vk->qfam, .queueCount = 1, .pQueuePriorities = &prio,
  };
  const char *dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
  VkDeviceCreateInfo dci = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
    .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts,
  };
  VK_CHECK (vkCreateDevice (vk->phys, &dci, NULL, &vk->dev));
  vkGetDeviceQueue (vk->dev, vk->qfam, 0, &vk->queue);

  /* UNORM, not sRGB: the reference phone (and GTK) blend in gamma space, and so must we to look the same */
  uint32_t nf = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR (vk->phys, vk->surface, &nf, NULL);
  VkSurfaceFormatKHR fmts[32];
  nf = MIN (nf, 32);
  vkGetPhysicalDeviceSurfaceFormatsKHR (vk->phys, vk->surface, &nf, fmts);
  vk->format = VK_FORMAT_UNDEFINED;
  for (uint32_t i = 0; i < nf && !vk->format; i++)
    if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM)
      vk->format = fmts[i].format;
  for (uint32_t i = 0; i < nf && !vk->format; i++)
    if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM)
      vk->format = fmts[i].format;
  if (!vk->format) {
    g_warning ("vulkan: no UNORM BGRA/RGBA surface format");
    return false;
  }

  vk->rp_present = make_render_pass (vk, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  vk->rp_offscreen = make_render_pass (vk, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  VkCommandPoolCreateInfo pci = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    .queueFamilyIndex = vk->qfam,
  };
  VK_CHECK (vkCreateCommandPool (vk->dev, &pci, NULL, &vk->cmd_pool));
  for (int i = 0; i < VK_FRAMES; i++) {
    VkCommandBufferAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = vk->cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VK_CHECK (vkAllocateCommandBuffers (vk->dev, &ai, &vk->frame[i].cmd));
    VkSemaphoreCreateInfo s = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_CHECK (vkCreateSemaphore (vk->dev, &s, NULL, &vk->frame[i].acquired));
    VkFenceCreateInfo f = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VK_CHECK (vkCreateFence (vk->dev, &f, NULL, &vk->frame[i].fence));
  }
  return make_swapchain (vk, w, h);
}

bool
vk_resize (Vk *vk, uint32_t w, uint32_t h)
{
  if (vk->extent.width == w && vk->extent.height == h && !vk->recreate)
    return true;
  vkDeviceWaitIdle (vk->dev);
  return make_swapchain (vk, w, h);
}

uint32_t
vk_memory_type (Vk *vk, uint32_t type_bits, VkMemoryPropertyFlags flags)
{
  for (uint32_t i = 0; i < vk->memp.memoryTypeCount; i++)
    if ((type_bits & (1u << i)) && (vk->memp.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  return UINT32_MAX;
}

bool
vk_alloc_buffer (Vk *vk, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                 VkBuffer *buf, VkDeviceMemory *mem)
{
  VkBufferCreateInfo bci = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VK_CHECK (vkCreateBuffer (vk->dev, &bci, NULL, buf));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements (vk->dev, *buf, &req);
  VkMemoryAllocateInfo mai = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = req.size,
    .memoryTypeIndex = vk_memory_type (vk, req.memoryTypeBits, flags),
  };
  VK_CHECK (vkAllocateMemory (vk->dev, &mai, NULL, mem));
  VK_CHECK (vkBindBufferMemory (vk->dev, *buf, *mem, 0));
  return true;
}

VkFrame *
vk_frame (Vk *vk)
{
  return &vk->frame[vk->frame_index % VK_FRAMES];
}

VkCommandBuffer
vk_frame_begin (Vk *vk)
{
  VkFrame *f = vk_frame (vk);
  vkWaitForFences (vk->dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
  if (vk->recreate && !vk_resize (vk, vk->extent.width, vk->extent.height))
    return VK_NULL_HANDLE;
  VkResult r = vkAcquireNextImageKHR (vk->dev, vk->swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE,
                                      &vk->image_index);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    vk->recreate = true;
    return VK_NULL_HANDLE;
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
    g_warning ("vulkan: acquire failed (%d)", r);
    return VK_NULL_HANDLE;
  }
  vkResetFences (vk->dev, 1, &f->fence);
  vkResetCommandBuffer (f->cmd, 0);
  VkCommandBufferBeginInfo bi = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer (f->cmd, &bi);
  return f->cmd;
}

bool
vk_frame_end (Vk *vk)
{
  VkFrame *f = vk_frame (vk);
  vkEndCommandBuffer (f->cmd);
  VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .waitSemaphoreCount = 1, .pWaitSemaphores = &f->acquired, .pWaitDstStageMask = &wait,
    .commandBufferCount = 1, .pCommandBuffers = &f->cmd,
    .signalSemaphoreCount = 1, .pSignalSemaphores = &vk->rendered[vk->image_index],
  };
  VK_CHECK (vkQueueSubmit (vk->queue, 1, &si, f->fence));
  VkPresentInfoKHR pi = {
    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    .waitSemaphoreCount = 1, .pWaitSemaphores = &vk->rendered[vk->image_index],
    .swapchainCount = 1, .pSwapchains = &vk->swapchain, .pImageIndices = &vk->image_index,
  };
  VkResult r = vkQueuePresentKHR (vk->queue, &pi);
  vk->frame_index++;
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    vk->recreate = true;
  return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

VkCommandBuffer
vk_oneshot_begin (Vk *vk)
{
  VkCommandBufferAllocateInfo ai = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = vk->cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
  };
  VkCommandBuffer cmd;
  vkAllocateCommandBuffers (vk->dev, &ai, &cmd);
  VkCommandBufferBeginInfo bi = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer (cmd, &bi);
  return cmd;
}

void
vk_oneshot_end (Vk *vk, VkCommandBuffer cmd)
{
  vkEndCommandBuffer (cmd);
  VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
  vkQueueSubmit (vk->queue, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle (vk->queue);
  vkFreeCommandBuffers (vk->dev, vk->cmd_pool, 1, &cmd);
}

void
vk_destroy (Vk *vk)
{
  if (!vk->dev)
    return;
  vkDeviceWaitIdle (vk->dev);
  destroy_swapchain_images (vk);
  vkDestroySwapchainKHR (vk->dev, vk->swapchain, NULL);
  for (int i = 0; i < VK_FRAMES; i++) {
    vkDestroySemaphore (vk->dev, vk->frame[i].acquired, NULL);
    vkDestroyFence (vk->dev, vk->frame[i].fence, NULL);
  }
  vkDestroyCommandPool (vk->dev, vk->cmd_pool, NULL);
  vkDestroyRenderPass (vk->dev, vk->rp_present, NULL);
  vkDestroyRenderPass (vk->dev, vk->rp_offscreen, NULL);
  vkDestroyDevice (vk->dev, NULL);
  vkDestroySurfaceKHR (vk->inst, vk->surface, NULL);
  vkDestroyInstance (vk->inst, NULL);
}
