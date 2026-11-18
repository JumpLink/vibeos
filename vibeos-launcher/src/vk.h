/* The Vulkan device and the swapchain on the launcher's Wayland surface, for gfx-vk.c. Vulkan 1.0 only: panvk on a
 * Mali-G76 (Bifrost v7) offers no more, so no dynamic rendering, timeline semaphores or descriptor indexing anywhere
 * in here. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct wl_display;
struct wl_surface;

#define VK_FRAMES     2           /* frames in flight */
#define VK_MAX_IMAGES 8

typedef struct {
  VkCommandBuffer cmd;
  VkSemaphore     acquired;
  VkFence         fence;
} VkFrame;

typedef struct {
  VkInstance                       inst;
  VkPhysicalDevice                 phys;
  VkPhysicalDeviceProperties       props;
  VkPhysicalDeviceMemoryProperties memp;
  VkDevice                         dev;
  uint32_t                         qfam;
  VkQueue                          queue;

  VkSurfaceKHR                     surface;
  VkSwapchainKHR                   swapchain;
  VkFormat                         format;          /* the swapchain's, and every offscreen target's */
  VkExtent2D                       extent;
  VkPresentModeKHR                 present_mode;
  uint32_t                         nimages;
  VkImage                          images[VK_MAX_IMAGES];
  VkImageView                      views[VK_MAX_IMAGES];
  VkFramebuffer                    fbs[VK_MAX_IMAGES];
  VkSemaphore                      rendered[VK_MAX_IMAGES];

  VkRenderPass                     rp_present;      /* clear, draw, present */
  VkRenderPass                     rp_offscreen;    /* clear, draw, sample afterwards (same format: compatible) */
  VkCommandPool                    cmd_pool;
  VkFrame                          frame[VK_FRAMES];
  uint32_t                         frame_index;     /* counts up; % VK_FRAMES is the slot */
  uint32_t                         image_index;
  bool                             recreate;
} Vk;

/* allow_cpu: a software rasterizer (lavapipe) will do when no GPU can present; without it, no GPU fails */
bool            vk_init          (Vk *vk, struct wl_display *display, struct wl_surface *surface, uint32_t w, uint32_t h,
                                  bool allow_cpu);
bool            vk_resize        (Vk *vk, uint32_t w, uint32_t h);
uint32_t        vk_memory_type   (Vk *vk, uint32_t type_bits, VkMemoryPropertyFlags flags);
bool            vk_alloc_buffer  (Vk *vk, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                                  VkBuffer *buf, VkDeviceMemory *mem);

/* A frame: begin returns the frame's command buffer (recording) or NULL when there is no image to draw into. */
VkCommandBuffer vk_frame_begin   (Vk *vk);
bool            vk_frame_end     (Vk *vk);
VkFrame        *vk_frame         (Vk *vk);

VkCommandBuffer vk_oneshot_begin (Vk *vk);
void            vk_oneshot_end   (Vk *vk, VkCommandBuffer cmd);

void            vk_destroy       (Vk *vk);     /* what vk_init made, all of it or as far as it got */

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    g_warning ("%s:%d: %s = %d", __FILE__, __LINE__, #x, r_); return false; } } while (0)
