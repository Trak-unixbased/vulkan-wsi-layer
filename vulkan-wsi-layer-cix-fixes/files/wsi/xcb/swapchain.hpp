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

#include <cstdint>
#include <condition_variable>
#include <mutex>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
extern "C" {
#include <X11/xshmfence.h>
}

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
      , pixmap(XCB_NONE)
      , present_fence()
      , shm_fence(nullptr)
      , sync_fence(XCB_NONE)
   {
   }

   external_memory external_mem;
   /** X pixmap wrapping the image dma-buf (DRI3). */
   xcb_pixmap_t pixmap;
   /** Fence signalled when the GPU has finished rendering the image. */
   sync_fd_fence_sync present_fence;
   /** Shared-memory fence triggered by the X server once it no longer reads the pixmap. */
   struct xshmfence *shm_fence;
   /** X Sync fence object backed by @ref shm_fence. */
   uint32_t sync_fence;
};

struct image_creation_parameters
{
   VkImageCreateInfo m_image_create_info;
   wsialloc_format m_allocated_format;
   util::vector<VkSubresourceLayout> m_image_layout;
   VkExternalMemoryImageCreateInfoKHR m_external_info;
   VkImageDrmFormatModifierExplicitCreateInfoEXT m_drm_mod_info;

   image_creation_parameters(wsialloc_format allocated_format, util::allocator allocator,
                             VkExternalMemoryImageCreateInfoKHR external_info,
                             VkImageDrmFormatModifierExplicitCreateInfoEXT drm_mod_info)
      : m_image_create_info()
      , m_allocated_format(allocated_format)
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
    * @brief Hook for any actions to free up a buffer for acquire
    *
    * @param[in,out] timeout time to wait, in nanoseconds. 0 doesn't block,
    *                        UINT64_MAX waits indefinitely.
    */
   VkResult get_free_buffer(uint64_t *timeout) override;

   /**
    * @brief Sets the present payload for a swapchain image.
    *
    * @param[in] image       The swapchain image for which to set a present payload.
    * @param     queue       A Vulkan queue that can be used for any Vulkan commands needed.
    * @param[in] semaphores  Array of Vulkan semaphores that constitute the payload.
    * @param[in] submission_pnext Chain of pointers to attach to the payload submission.
    *
    * @return VK_SUCCESS on success or an error code otherwise.
    */
   VkResult image_set_present_payload(swapchain_image &image, VkQueue queue, const queue_submit_semaphores &semaphores,
                                      const void *submission_pnext) override;

   VkResult image_wait_present(swapchain_image &image, uint64_t timeout) override;

   /**
    * @brief Process the X Present events already received, without blocking.
    */
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
   xcb_window_t m_window;

   /** Special event queue receiving the Present events of @ref m_window, or nullptr. */
   xcb_special_event_t *m_present_event;
   /** Event context id used with xcb_present_select_input. */
   uint32_t m_event_id;

   /**
    * @brief Serialises the processing of Present events.
    *
    * Events are consumed both by the page flip thread (FIFO pacing) and by the application thread (waiting for a
    * free image). Everything below is protected by this mutex.
    */
   std::mutex m_event_mutex;
   /** Signalled whenever Present events have been processed, wakes threads waiting on @ref m_event_mutex. */
   std::condition_variable m_event_cv;
   /** True while one thread is blocked in poll() on the X connection. */
   bool m_event_poller_active;
   /** MSC of the last completed present. */
   uint64_t m_last_present_msc;

   uint32_t m_window_width;  // width of m_window when the swapchain was created
   uint32_t m_window_height; // height of m_window when the swapchain was created

   /** XCB_PRESENT_OPTION_COPY forced through DISABLE_XCB_FLIP. Read once at creation time. */
   bool m_force_copy;
   /** Use XCB_PRESENT_OPTION_ASYNC for MAILBOX (only on Xwayland, where async flips do not tear). */
   bool m_mailbox_async;

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

   /**
    * @brief Allocates the memory of a swapchain image using the format selected at swapchain creation.
    */
   VkResult allocate_image(x11_image_data *image_data);

   /**
    * @brief Allocate a buffer from the WSI Allocator.
    *
    * @param image_create_info  The create info for the Vulkan image.
    * @param image_data         The X11 image data.
    * @param importable_formats The list of importable formats.
    * @param allocated_format   The allocated format.
    * @param avoid_allocation   Only select the format, do not allocate memory.
    * @return VkResult The result of the allocation.
    */
   VkResult allocate_wsialloc(const VkImageCreateInfo &image_create_info, x11_image_data *image_data,
                              util::vector<wsialloc_format> &importable_formats, wsialloc_format *allocated_format,
                              bool avoid_allocation);

   /**
    * @brief Create the X pixmap and the X Sync fence of a swapchain image (DRI3).
    *
    * @param image_create_info   The create info for the Vulkan image.
    * @param image_data          The X11 image data.
    * @param creation_parameters The parameters for image creation.
    * @return VkResult The result of the image initialization.
    */
   VkResult x11_image_init(const VkImageCreateInfo &image_create_info, x11_image_data *image_data,
                           const image_creation_parameters &creation_parameters);

   /**
    * @brief Finds what formats are compatible with the requested swapchain image Vulkan Device and Xcb surface.
    *
    * @param      info               The Swapchain image creation info.
    * @param[out] importable_formats A list of formats that can be imported to the Vulkan Device.
    * @param[out] exportable_modifers A list of modifiers that can be exported from the Vulkan Device.
    * @param[out] drm_format_props   The DRM format modifier properties of the Vulkan format.
    *
    * @return VK_SUCCESS or VK_ERROR_OUT_OF_HOST_MEMORY
    */
   VkResult get_surface_compatible_formats(const VkImageCreateInfo &info,
                                           util::vector<wsialloc_format> &importable_formats,
                                           util::vector<uint64_t> &exportable_modifers,
                                           util::vector<VkDrmFormatModifierPropertiesEXT> &drm_format_props);

   /**
    * @brief Handle one Present event and free it. Must be called with @ref m_event_mutex held.
    */
   void handle_present_event_locked(xcb_present_generic_event_t *event);

   /**
    * @brief Process all queued Present events without blocking. Must be called with @ref m_event_mutex held.
    *
    * @return true if at least one event was processed.
    */
   bool drain_present_events_locked();

   /**
    * @brief Wait until @p done returns true, processing Present events meanwhile.
    *
    * @param abs_timeout Absolute CLOCK_MONOTONIC deadline in nanoseconds, UINT64_MAX for no deadline.
    * @param done        Predicate evaluated with @ref m_event_mutex held.
    *
    * @return VK_SUCCESS, VK_TIMEOUT, or an error if the window or the connection is gone.
    */
   template <typename Predicate>
   VkResult wait_for_present_events(uint64_t abs_timeout, Predicate done);

   /**
    * @brief Returns true if at least one image is FREE.
    */
   bool free_image_found();

   /**
    * @brief Return all images still owned by the X server to the FREE state (window or connection lost).
    */
   void release_server_owned_images();
};
} // namespace xcb
} // namespace wsi
