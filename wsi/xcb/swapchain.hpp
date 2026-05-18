/*
 * Copyright 2026 Cix Technology Group Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright (c) 2017-2019, 2021-2022 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include "wsi/swapchain_base.hpp"

extern "C" {
#include <vulkan/vk_icd.h>
}

#include <X11/Xlib-xcb.h>
extern "C" {
#include <X11/xshmfence.h>
}
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shm.h>

#include "util/wsialloc/wsialloc.h"
#include "util/custom_allocator.hpp"
#include "surface.hpp"
#include "wsi/external_memory.hpp"

namespace wsi
{
namespace xcb
{

struct x11_image_data
{
   x11_image_data(const VkDevice &device, const util::allocator &allocator)
      : external_mem(device, allocator)
      , pixmap(0)
      , present_fence()
      , shm_fence(nullptr)
      , sync_fence(0)
      , serial(0)
      , shmseg(0)
      , shmid(-1)
   {
   };

   external_memory    external_mem;
   xcb_pixmap_t       pixmap;
   sync_fd_fence_sync present_fence;
   struct xshmfence  *shm_fence;
   uint32_t           sync_fence;
   uint32_t           serial;
   xcb_shm_seg_t      shmseg;
   int                shmid;
};

struct image_creation_parameters
{
   VkImageCreateInfo m_image_create_info;
   wsialloc_format m_allocated_format;
   util::vector<VkSubresourceLayout> m_image_layout;
   VkExternalMemoryImageCreateInfoKHR m_external_info;
   VkImageDrmFormatModifierExplicitCreateInfoEXT m_drm_mod_info;

   image_creation_parameters(wsialloc_format allocated_format,
                             util::allocator allocator, VkExternalMemoryImageCreateInfoKHR external_info,
                             VkImageDrmFormatModifierExplicitCreateInfoEXT drm_mod_info)
      : m_allocated_format(allocated_format)
      , m_image_layout(allocator)
      , m_external_info(external_info)
      , m_drm_mod_info(drm_mod_info)
   {
   }
};

class swapchain : public wsi::swapchain_base
{
public:
   explicit swapchain(layer::device_private_data &dev_data, const VkAllocationCallbacks *allocator,
                      surface &wsi_surface);

   ~swapchain();

   /* TODO: make the buffer destructor a friend? so this can be protected */
   void release_buffer();

protected:
   /**
    * @brief Initialize platform specifics.
    */
   VkResult init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                          bool &use_presentation_thread) override;

   /**
    * @brief Allocates and binds a new swapchain image.
    *
    * @param image_create_info Data to be used to create the image.
    * @param image             Handle to the image.
    *
    * @return Returns VK_SUCCESS on success, otherwise an appropriate error code.
    */
   VkResult allocate_and_bind_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image) override;

   /**
    * @brief Creates a new swapchain image.
    *
    * @param image_create_info Data to be used to create the image.
    * @param image             Handle to the image.
    *
    * @return If image creation is successful returns VK_SUCCESS, otherwise
    * will return VK_ERROR_OUT_OF_DEVICE_MEMORY or VK_ERROR_INITIALIZATION_FAILED
    * depending on the error that occurred.
    */
   VkResult create_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image) override;

   /**
    * @brief Method to present an image
    *
    * It sends the next image for presentation to the presentation engine.
    *
    * @param pending_present Information on the pending present request.
    */
   void present_image(const pending_present_request &pending_present) override;

   /**
    * @brief Method to release a swapchain image
    *
    * @param image Handle to the image about to be released.
    */
   void destroy_image(swapchain_image &image) override;

   /**
    * @brief Method to check if there are any free images
    *
    * @return true if any images are free, otherwise false.
    */
   bool free_image_found();

   /**
    * @brief Hook for any actions to free up a buffer for acquire
    *
    * @param[in,out] timeout time to wait, in nanoseconds. 0 doesn't block,
    *                        UINT64_MAX waits indefinitely. The timeout should
    *                        be updated if a sleep is required - this can
    *                        be set to 0 if the semaphore is now not expected
    *                        block.
    */
   VkResult get_free_buffer(uint64_t *timeout) override;

   /**
    * @brief Sets the present payload for a swapchain image.
    *
    * @param[in] image       The swapchain image for which to set a present payload.
    * @param     queue       A Vulkan queue that can be used for any Vulkan commands needed.
    * @param[in] sem_payload Array of Vulkan semaphores that constitute the payload.
    * @param[in] submission_pnext Chain of pointers to attach to the payload submission.
    *
    * @return VK_SUCCESS on success or an error code otherwise.
    */
   VkResult image_set_present_payload(swapchain_image &image, VkQueue queue, const queue_submit_semaphores &semaphores,
                                      const void *submission_pnext) override;

   VkResult image_wait_present(swapchain_image &image, uint64_t timeout) override;

   void update_pending_image_status() override;

   /**
    * @brief Bind image to a swapchain
    *
    * @param device              is the logical device that owns the images and memory.
    * @param bind_image_mem_info details the image we want to bind.
    * @param bind_sc_info        describes the swapchain memory to bind to.
    *
    * @return VK_SUCCESS on success, otherwise on failure VK_ERROR_OUT_OF_HOST_MEMORY or VK_ERROR_OUT_OF_DEVICE_MEMORY
    * can be returned.
    */
   VkResult bind_swapchain_image(VkDevice &device, const VkBindImageMemoryInfo *bind_image_mem_info,
                                 const VkBindImageMemorySwapchainInfoKHR *bind_sc_info) override;

private:
   xcb_connection_t *m_connection;
   xcb_window_t      m_window;
   xcb_gc_t          m_gc;
   xcb_screen_t     *m_screen;
   xcb_pixmap_t      m_backing_pixmap;
   xcb_special_event_t *m_present_event;
   xcb_present_event_t m_event_id;

   uint64_t m_last_present_msc;

   uint32_t m_window_width;                // width of m_window
   uint32_t m_window_height;               // height of m_window

   /**
    * @brief Allocates a Vulkan image with the specified image create info.
    *
    * @param image_create_info The create info for the Vulkan image.
    * @param image_data The x11_image_data structure to store the image data.
    * @return VkResult The result of the image allocation operation.
    */
   VkResult allocate_image(VkImageCreateInfo &image_create_info,
                           x11_image_data *image_data);

   /**
    * @brief Allocate a buffer from the WSI Allocator.
    *
    * This function allocates image resource, like importable formats, and allocated format.
    *
    * @param image_create_info The create info for the Vulkan image.
    * @param image_data The X11 image data.
    * @param importable_formats The list of importable formats.
    * @param allocated_format The allocated format.
    * @param avoid_allocation Indicates that the image should be real allocated.
    * @return VkResult The result of the allocation.
    */
   VkResult allocate_wsialloc(VkImageCreateInfo &image_create_info,
                              x11_image_data *image_data,
                              util::vector<wsialloc_format> &importable_formats,
                              wsialloc_format *allocated_format,
                              bool avoid_allocation);

   /**
    * @brief Notifi xwayland to initializes an surface image.
    *
    * @param image_create_info The create info for the Vulkan image.
    * @param image_data The X11 image data.
    * @param creation_parameters The parameters for image creation.
    * @return VkResult The result of the image initialization.
    */
   VkResult x11_image_init(VkImageCreateInfo         image_create_info,
                           x11_image_data            *image_data,
                           image_creation_parameters &creation_parameters);

   /** Raw pointer to the WSI Surface that this swapchain was created from. The Vulkan specification ensures that the
    * surface is valid until swapchain is destroyed. */
   surface *m_wsi_surface;

   /**
    * @brief Handle to the WSI allocator.
    */
   wsialloc_allocator *m_wsi_allocator;

   /**
    * @brief Image creation parameters used for all swapchain images.
    */
   struct image_creation_parameters m_image_creation_parameters;

   /*
    * @brief Get the memory type which the specified file descriptor can be
    * imported as.
    *
    * @param      fd      The given fd.
    * @param[out] mem_idx The index of the supported memory type.
    *
    * @return VK_SUCCESS on success. On failure the error value of
    * vkGetMemoryFdPropertiesKHR is returned.
    */
   VkResult get_fd_mem_type_index(int fd, uint32_t &mem_idx);

   /**
    * @brief Finds what formats are compatible with the requested swapchain image Vulkan Device and Xcb surface.
    *
    * @param      info               The Swapchain image creation info.
    * @param[out] importable_formats A list of formats that can be imported to the Vulkan Device.
    * @param[out] exportable_formats A list of formats that can be exported from the Vulkan Device.
    *
    * @return VK_SUCCESS or VK_ERROR_OUT_OF_HOST_MEMORY
    */
   VkResult get_surface_compatible_formats(const VkImageCreateInfo &info,
                                           util::vector<wsialloc_format> &importable_formats,
                                           util::vector<uint64_t> &exportable_modifers,
                                           util::vector<VkDrmFormatModifierPropertiesEXT> &drm_format_props);
   void handle_dri3_present_event(xcb_present_generic_event_t *event);

   void poll_present_event_for_pending_image();
};
} // namespace xcb
} // namespace wsi
