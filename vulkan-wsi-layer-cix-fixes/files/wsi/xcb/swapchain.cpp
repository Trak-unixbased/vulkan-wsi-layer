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

#define VK_USE_PLATFORM_XCB_KHR 1
#define VK_USE_PLATFORM_XLIB_KHR 1
#include "swapchain.hpp"
#include "surface_properties.hpp"

#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include <xcb/sync.h>
#include <drm_fourcc.h>

#include "util/drm/drm_utils.hpp"
#include "util/log.hpp"
#include "util/helpers.hpp"
#include "util/macros.hpp"
#include "util/format_modifiers.hpp"

#include "xcb_helpers.hpp"

/* XXX this belongs in presentproto */
#ifndef PresentWindowDestroyed
#define PresentWindowDestroyed (1 << 0)
#endif

namespace wsi
{
namespace xcb
{

namespace
{

constexpr uint64_t NSEC_PER_MSEC = 1000000ull;
/* poll() granularity: start at 1 ms, grow up to 10 ms while nothing happens. */
constexpr uint64_t POLL_INTERVAL_MIN_NS = 1 * NSEC_PER_MSEC;
constexpr uint64_t POLL_INTERVAL_MAX_NS = 10 * NSEC_PER_MSEC;
/* Without any Present event for this long, check (one round trip) that the window still exists. A destroyed window
 * never sends the events we are waiting for (e.g. Zink destroys the X window before the Vulkan swapchain). */
constexpr uint64_t WINDOW_CHECK_INTERVAL_NS = 250 * NSEC_PER_MSEC;

uint64_t monotonic_time_ns()
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t absolute_timeout_ns(uint64_t timeout)
{
   if (timeout == UINT64_MAX)
   {
      return UINT64_MAX;
   }
   const uint64_t now = monotonic_time_ns();
   /* Saturate instead of wrapping around for very large finite timeouts. */
   return (timeout > UINT64_MAX - now) ? UINT64_MAX : now + timeout;
}

int dup_cloexec(int fd)
{
   return fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

} // namespace

swapchain::swapchain(layer::device_private_data &dev_data, const VkAllocationCallbacks *pAllocator,
                     surface &wsi_surface)
   : swapchain_base(dev_data, pAllocator)
   , m_connection(wsi_surface.get_xcb_connection())
   , m_window(wsi_surface.get_xcb_window())
   , m_present_event(nullptr)
   , m_event_id(0)
   , m_event_poller_active(false)
   , m_last_present_msc(0)
   , m_window_width(0)
   , m_window_height(0)
   , m_force_copy(false)
   , m_mailbox_async(false)
   , m_wsi_surface(&wsi_surface)
   , m_wsi_allocator(nullptr)
   , m_image_creation_parameters({}, m_allocator, {}, {})
{
   m_image_create_info.format = VK_FORMAT_UNDEFINED;
}

swapchain::~swapchain()
{
   teardown();

   /* m_present_event is only valid if init_platform() got far enough. */
   if (m_present_event != nullptr)
   {
      xcb_unregister_for_special_event(m_connection, m_present_event);
      m_present_event = nullptr;

      /* Checked + discarded: a BadWindow (window already destroyed) must not reach the application's event loop. */
      const xcb_void_cookie_t cookie =
         xcb_present_select_input_checked(m_connection, m_event_id, m_window, XCB_PRESENT_EVENT_MASK_NO_EVENT);
      xcb_discard_reply(m_connection, cookie.sequence);
      xcb_flush(m_connection);
   }

   if (m_wsi_allocator != nullptr)
   {
      wsialloc_delete(m_wsi_allocator);
   }
   m_wsi_allocator = nullptr;
}

VkResult swapchain::init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                                  bool &use_presentation_thread)
{
   UNUSED(device);

   const connection_caps &caps = m_wsi_surface->get_connection_caps();
   if (!caps.has_dri3 || !caps.has_present)
   {
      WSI_LOG_ERROR("X server lacks DRI3 or Present, cannot create a swapchain.");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   m_window_width = swapchain_create_info->imageExtent.width;
   m_window_height = swapchain_create_info->imageExtent.height;
   WSI_LOG_INFO("Swapchain min image count is %u.", swapchain_create_info->minImageCount);

   /* Select the Present events of the window. This is the only request checked synchronously: if the window is
    * already gone there is no point in going further. */
   m_event_id = xcb_generate_id(m_connection);
   const xcb_void_cookie_t cookie =
      xcb_present_select_input_checked(m_connection, m_event_id, m_window,
                                       XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY |
                                          XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY | XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
   m_present_event = xcb_register_for_special_xge(m_connection, &xcb_present_id, m_event_id, nullptr);

   xcb_generic_error_t *error = xcb_request_check(m_connection, cookie);
   if (error != nullptr)
   {
      WSI_LOG_ERROR("Failed to select Present events on the window (X error %d).", error->error_code);
      free(error);
      return VK_ERROR_SURFACE_LOST_KHR;
   }
   if (m_present_event == nullptr)
   {
      WSI_LOG_ERROR("Failed to register for Present events.");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   WSIALLOC_ASSERT_VERSION();
   if (wsialloc_new(&m_wsi_allocator) != WSIALLOC_ERROR_NONE)
   {
      WSI_LOG_ERROR("Failed to create wsi allocator.");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   set_adaptive_sync_property(m_connection, m_window, 0);

   /* Environment is read once here instead of on every present. */
   m_force_copy = (getenv("DISABLE_XCB_FLIP") != nullptr);

   /* XCB_PRESENT_OPTION_ASYNC gives MAILBOX semantics on Xwayland (the compositor never tears), but tears on a native
    * Xorg server, where the Present extension already replaces queued presents without it.
    * WSI_XCB_MAILBOX_ASYNC=0/1 overrides the detection. */
   m_mailbox_async = caps.is_xwayland;
   const char *async_env = getenv("WSI_XCB_MAILBOX_ASYNC");
   if (async_env != nullptr)
   {
      m_mailbox_async = (strcmp(async_env, "0") != 0);
   }

   use_presentation_thread = (getenv("DISABLE_XCB_PRESENT_THREAD") == nullptr);

   return VK_SUCCESS;
}

VkResult swapchain::get_surface_compatible_formats(const VkImageCreateInfo &info,
                                                   util::vector<wsialloc_format> &importable_formats,
                                                   util::vector<uint64_t> &exportable_modifers,
                                                   util::vector<VkDrmFormatModifierPropertiesEXT> &drm_format_props)
{
   TRY_LOG(util::get_drm_format_properties(m_device_data.physical_device, info.format, drm_format_props),
           "Failed to get format properties");

   const uint32_t fourcc = util::drm::vk_to_drm_format(info.format);

   for (const auto &prop : drm_format_props)
   {
      bool is_supported = false;
      for (const auto &format : m_wsi_surface->get_formats())
      {
         if (format.fourcc == fourcc && format.modifier == prop.drmFormatModifier)
         {
            is_supported = true;
            break;
         }
      }
      if (!is_supported)
      {
         continue;
      }

      VkExternalImageFormatPropertiesKHR external_props = {};
      external_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR;

      VkImageFormatProperties2KHR format_props = {};
      format_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR;
      format_props.pNext = &external_props;

      VkPhysicalDeviceExternalImageFormatInfoKHR external_info = {};
      external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR;
      external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

      VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_mod_info = {};
      drm_mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
      drm_mod_info.pNext = &external_info;
      drm_mod_info.drmFormatModifier = prop.drmFormatModifier;
      drm_mod_info.sharingMode = info.sharingMode;
      drm_mod_info.queueFamilyIndexCount = info.queueFamilyIndexCount;
      drm_mod_info.pQueueFamilyIndices = info.pQueueFamilyIndices;

      VkPhysicalDeviceImageFormatInfo2KHR image_info = {};
      image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR;
      image_info.pNext = &drm_mod_info;
      image_info.format = info.format;
      image_info.type = info.imageType;
      image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      image_info.usage = info.usage;
      image_info.flags = info.flags;

#if WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN
      VkImageCompressionControlEXT compression_control = {};
      compression_control.sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT;
      compression_control.flags = m_image_compression_control_params.flags;
      compression_control.compressionControlPlaneCount =
         m_image_compression_control_params.compression_control_plane_count;
      compression_control.pFixedRateFlags = m_image_compression_control_params.fixed_rate_flags.data();

      if (m_device_data.is_swapchain_compression_control_enabled())
      {
         compression_control.pNext = image_info.pNext;
         image_info.pNext = &compression_control;
      }
#endif
      const VkResult result = m_device_data.instance_data.disp.GetPhysicalDeviceImageFormatProperties2KHR(
         m_device_data.physical_device, &image_info, &format_props);
      if (result != VK_SUCCESS)
      {
         continue;
      }
      if (format_props.imageFormatProperties.maxExtent.width < info.extent.width ||
          format_props.imageFormatProperties.maxExtent.height < info.extent.height ||
          format_props.imageFormatProperties.maxExtent.depth < info.extent.depth)
      {
         continue;
      }
      if (format_props.imageFormatProperties.maxMipLevels < info.mipLevels ||
          format_props.imageFormatProperties.maxArrayLayers < info.arrayLayers)
      {
         continue;
      }
      if ((format_props.imageFormatProperties.sampleCounts & info.samples) != info.samples)
      {
         continue;
      }

      if (external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT_KHR)
      {
         if (!exportable_modifers.try_push_back(prop.drmFormatModifier))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }

      if (external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR)
      {
         const uint64_t flags =
            (prop.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_DISJOINT_BIT) ? 0 : WSIALLOC_FORMAT_NON_DISJOINT;
         const wsialloc_format import_format{ fourcc, prop.drmFormatModifier, flags };
         if (!importable_formats.try_push_back(import_format))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }
   }

   return VK_SUCCESS;
}

VkResult swapchain::allocate_wsialloc(const VkImageCreateInfo &image_create_info, x11_image_data *image_data,
                                      util::vector<wsialloc_format> &importable_formats,
                                      wsialloc_format *allocated_format, bool avoid_allocation)
{
   const bool is_protected_memory = (image_create_info.flags & VK_IMAGE_CREATE_PROTECTED_BIT) != 0;
   uint64_t allocation_flags = is_protected_memory ? WSIALLOC_ALLOCATE_PROTECTED : 0;
   if (avoid_allocation)
   {
      allocation_flags |= WSIALLOC_ALLOCATE_NO_MEMORY;
   }

#if WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN
   if (m_image_compression_control_params.flags & VK_IMAGE_COMPRESSION_FIXED_RATE_EXPLICIT_EXT)
   {
      allocation_flags |= WSIALLOC_ALLOCATE_HIGHEST_FIXED_RATE_COMPRESSION;
   }
#endif

   const wsialloc_allocate_info alloc_info = { importable_formats.data(),
                                               static_cast<unsigned>(importable_formats.size()),
                                               image_create_info.extent.width, image_create_info.extent.height,
                                               allocation_flags };

   wsialloc_allocate_result alloc_result = {};
   /* Clear buffer_fds and average_row_strides for error purposes */
   for (int i = 0; i < WSIALLOC_MAX_PLANES; ++i)
   {
      alloc_result.buffer_fds[i] = -1;
      alloc_result.average_row_strides[i] = -1;
   }
   const auto res = wsialloc_alloc(m_wsi_allocator, &alloc_info, &alloc_result);
   if (res != WSIALLOC_ERROR_NONE)
   {
      WSI_LOG_ERROR("Failed allocation of DMA Buffer. WSI error: %d", static_cast<int>(res));
      if (res == WSIALLOC_ERROR_NOT_SUPPORTED)
      {
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *allocated_format = alloc_result.format;
   auto &external_memory = image_data->external_mem;
   external_memory.set_strides(alloc_result.average_row_strides);
   external_memory.set_buffer_fds(alloc_result.buffer_fds);
   external_memory.set_offsets(alloc_result.offsets);

   const uint32_t num_planes = util::drm::drm_fourcc_format_get_num_planes(alloc_result.format.fourcc);

   if (!avoid_allocation)
   {
      uint32_t num_memory_planes = 0;

      for (uint32_t i = 0; i < num_planes; ++i)
      {
         auto it = std::find(std::begin(alloc_result.buffer_fds) + i + 1, std::end(alloc_result.buffer_fds),
                             alloc_result.buffer_fds[i]);
         if (it == std::end(alloc_result.buffer_fds))
         {
            num_memory_planes++;
         }
      }

      assert(alloc_result.is_disjoint == (num_memory_planes > 1));
      external_memory.set_num_memories(num_memory_planes);
   }

   external_memory.set_format_info(alloc_result.is_disjoint, num_planes);
   external_memory.set_memory_handle_type(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   return VK_SUCCESS;
}

static VkResult fill_image_create_info(VkImageCreateInfo &image_create_info,
                                       util::vector<VkSubresourceLayout> &image_plane_layouts,
                                       VkImageDrmFormatModifierExplicitCreateInfoEXT &drm_mod_info,
                                       VkExternalMemoryImageCreateInfoKHR &external_info, x11_image_data &image_data,
                                       uint64_t modifier)
{
   TRY_LOG_CALL(image_data.external_mem.fill_image_plane_layouts(image_plane_layouts));

   if (image_data.external_mem.is_disjoint())
   {
      image_create_info.flags |= VK_IMAGE_CREATE_DISJOINT_BIT;
   }

   image_data.external_mem.fill_drm_mod_info(image_create_info.pNext, drm_mod_info, image_plane_layouts, modifier);
   image_data.external_mem.fill_external_info(external_info, &drm_mod_info);
   image_create_info.pNext = &external_info;
   image_create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
   return VK_SUCCESS;
}

VkResult swapchain::allocate_image(x11_image_data *image_data)
{
   util::vector<wsialloc_format> importable_formats(util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
   auto &allocated_format = m_image_creation_parameters.m_allocated_format;

   if (!importable_formats.try_push_back(allocated_format))
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   TRY_LOG_CALL(allocate_wsialloc(m_image_create_info, image_data, importable_formats, &allocated_format, false));

   return VK_SUCCESS;
}

VkResult swapchain::x11_image_init(const VkImageCreateInfo &image_create_info, x11_image_data *image_data,
                                   const image_creation_parameters &creation_parameters)
{
   auto &ext_mem = image_data->external_mem;
   const auto &buffer_fds = ext_mem.get_buffer_fds();
   const auto &strides = ext_mem.get_strides();
   const auto &offsets = ext_mem.get_offsets();
   const uint32_t num_planes = ext_mem.get_num_planes();
   const uint64_t modifier = creation_parameters.m_allocated_format.modifier;
   const connection_caps &caps = m_wsi_surface->get_connection_caps();

   if (num_planes == 0 || num_planes > 4 || buffer_fds[0] < 0)
   {
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (image_create_info.extent.width > UINT16_MAX || image_create_info.extent.height > UINT16_MAX)
   {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   const uint8_t depth = static_cast<uint8_t>(m_wsi_surface->get_bit_depth());
   const uint8_t bpp = static_cast<uint8_t>(m_wsi_surface->get_bpp());
   const auto width = static_cast<uint16_t>(image_create_info.extent.width);
   const auto height = static_cast<uint16_t>(image_create_info.extent.height);

   xcb_pixmap_t pixmap = xcb_generate_id(m_connection);
   xcb_void_cookie_t cookie = {};

   if (caps.has_dri3_modifiers)
   {
      /* DRI3 >= 1.2: one fd per plane. XCB takes ownership of the fds and closes them once sent. */
      int32_t fds[4] = { -1, -1, -1, -1 };
      uint32_t plane_strides[4] = { 0, 0, 0, 0 };
      uint32_t plane_offsets[4] = { 0, 0, 0, 0 };
      for (uint32_t i = 0; i < num_planes; i++)
      {
         /* Disjoint images have one dma-buf per plane; the previous code always duplicated plane 0's fd. */
         const int source_fd = (buffer_fds[i] >= 0) ? buffer_fds[i] : buffer_fds[0];
         fds[i] = dup_cloexec(source_fd);
         if (fds[i] < 0)
         {
            for (uint32_t j = 0; j < i; j++)
            {
               close(fds[j]);
            }
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
         plane_strides[i] = static_cast<uint32_t>(strides[i]);
         plane_offsets[i] = offsets[i];
      }

      cookie = xcb_dri3_pixmap_from_buffers_checked(m_connection, pixmap, m_window, static_cast<uint8_t>(num_planes),
                                                    width, height, plane_strides[0], plane_offsets[0], plane_strides[1],
                                                    plane_offsets[1], plane_strides[2], plane_offsets[2],
                                                    plane_strides[3], plane_offsets[3], depth, bpp, modifier, fds);
   }
   else
   {
      /* DRI3 1.0/1.1: single plane, implicit (linear) layout only. */
      if (num_planes != 1 || modifier != DRM_FORMAT_MOD_LINEAR || strides[0] <= 0 || strides[0] > UINT16_MAX)
      {
         WSI_LOG_ERROR("Buffer layout not supported by the X server (DRI3 < 1.2).");
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
      const uint64_t size = static_cast<uint64_t>(height) * static_cast<uint64_t>(strides[0]);
      if (size > UINT32_MAX)
      {
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }

      const int fd = dup_cloexec(buffer_fds[0]);
      if (fd < 0)
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      cookie = xcb_dri3_pixmap_from_buffer_checked(m_connection, pixmap, m_window, static_cast<uint32_t>(size), width,
                                                   height, static_cast<uint16_t>(strides[0]), depth, bpp, fd);
   }

   /* One round trip per image, at creation time only. The previous code called xcb_discard_reply() before
    * xcb_request_check(), which discarded the error and made every failure look like a success. */
   xcb_generic_error_t *error = xcb_request_check(m_connection, cookie);
   if (error != nullptr)
   {
      WSI_LOG_ERROR("Could not create pixmap from buffer (X error %d).", error->error_code);
      free(error);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   image_data->pixmap = pixmap;

   /* Idle fence: triggered by the X server once it no longer reads the pixmap. */
   const int fence_fd = xshmfence_alloc_shm();
   if (fence_fd < 0)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   image_data->shm_fence = xshmfence_map_shm(fence_fd);
   if (image_data->shm_fence == nullptr)
   {
      close(fence_fd);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   image_data->sync_fence = xcb_generate_id(m_connection);
   /* XCB takes ownership of fence_fd. */
   xcb_dri3_fence_from_fd(m_connection, image_data->pixmap, image_data->sync_fence, false, fence_fd);
   xshmfence_trigger(image_data->shm_fence);
   xcb_flush(m_connection);

   return VK_SUCCESS;
}

VkResult swapchain::allocate_and_bind_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   UNUSED(image_create_info);

   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   image.status = swapchain_image::FREE;

   assert(image.data != nullptr);
   auto image_data = static_cast<x11_image_data *>(image.data);
   TRY_LOG(allocate_image(image_data), "Failed to allocate image");

   image_status_lock.unlock();

   /* m_image_create_info holds the final format/extent selected in create_swapchain_image(). */
   TRY_LOG(x11_image_init(m_image_create_info, image_data, m_image_creation_parameters), "Failed to init x11 image.");

   TRY_LOG(image_data->external_mem.import_memory_and_bind_swapchain_image(image.image),
           "Failed to import memory and bind swapchain image");

   /* Initialize presentation fence. */
   auto present_fence = sync_fd_fence_sync::create(m_device_data);
   if (!present_fence.has_value())
   {
      /* Resources are released by destroy_image() when the swapchain is torn down. */
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   image_data->present_fence = std::move(present_fence.value());

   return VK_SUCCESS;
}

VkResult swapchain::create_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   /* Create image_data */
   auto image_data = m_allocator.create<x11_image_data>(1, m_device, m_allocator);
   if (image_data == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   image.data = image_data;

   if (m_image_create_info.format == VK_FORMAT_UNDEFINED)
   {
      util::vector<wsialloc_format> importable_formats(util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
      util::vector<uint64_t> exportable_modifiers(util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));

      /* Query supported modifers. */
      util::vector<VkDrmFormatModifierPropertiesEXT> drm_format_props(
         util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));

      TRY_LOG_CALL(
         get_surface_compatible_formats(image_create_info, importable_formats, exportable_modifiers, drm_format_props));

      if (importable_formats.empty())
      {
         WSI_LOG_ERROR("Export/Import not supported.");
         return VK_ERROR_INITIALIZATION_FAILED;
      }

      wsialloc_format allocated_format = {};
      TRY_LOG_CALL(allocate_wsialloc(image_create_info, image_data, importable_formats, &allocated_format, true));

      for (const auto &prop : drm_format_props)
      {
         if (prop.drmFormatModifier == allocated_format.modifier)
         {
            image_data->external_mem.set_num_memories(prop.drmFormatModifierPlaneCount);
         }
      }

      TRY_LOG_CALL(fill_image_create_info(
         image_create_info, m_image_creation_parameters.m_image_layout, m_image_creation_parameters.m_drm_mod_info,
         m_image_creation_parameters.m_external_info, *image_data, allocated_format.modifier));

      m_image_create_info = image_create_info;
      m_image_creation_parameters.m_allocated_format = allocated_format;
   }

   return m_device_data.disp.CreateImage(m_device, &m_image_create_info, get_allocation_callbacks(), &image.image);
}

void swapchain::release_server_owned_images()
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   for (uint32_t i = 0; i < m_swapchain_images.size(); i++)
   {
      const auto status = m_swapchain_images[i].status;
      /* Only images held by the presentation engine: ACQUIRED images belong to the application. */
      if (status == swapchain_image::PENDING || status == swapchain_image::PRESENTED)
      {
         unpresent_image(i);
      }
   }
}

void swapchain::handle_present_event_locked(xcb_present_generic_event_t *event)
{
   switch (event->evtype)
   {
   case XCB_PRESENT_CONFIGURE_NOTIFY:
   {
      auto *config = reinterpret_cast<xcb_present_configure_notify_event_t *>(event);
      if (config->pixmap_flags & PresentWindowDestroyed)
      {
         WSI_LOG_INFO("X window destroyed.");
         set_error_state(VK_ERROR_SURFACE_LOST_KHR);
         release_server_owned_images();
      }
      else if (config->width != m_window_width || config->height != m_window_height)
      {
         WSI_LOG_INFO("Window size changed!");
         /* Never overrides a real error, see swapchain_base::set_error_state(). */
         set_error_state(VK_SUBOPTIMAL_KHR);
      }
      break;
   }
   case XCB_PRESENT_EVENT_IDLE_NOTIFY:
   {
      auto *idle = reinterpret_cast<xcb_present_idle_notify_event_t *>(event);
      std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
      for (uint32_t i = 0; i < m_swapchain_images.size(); i++)
      {
         auto &img = m_swapchain_images[i];
         if (img.data == nullptr ||
             (img.status != swapchain_image::PENDING && img.status != swapchain_image::PRESENTED))
         {
            continue;
         }
         auto *image_data = reinterpret_cast<x11_image_data *>(img.data);
         if (image_data->pixmap == idle->pixmap)
         {
            /* Make sure X has released that image before the driver reuses it. */
            xshmfence_await(image_data->shm_fence);
            unpresent_image(i);
            break;
         }
      }
      break;
   }
   case XCB_PRESENT_EVENT_COMPLETE_NOTIFY:
   {
      auto *complete = reinterpret_cast<xcb_present_complete_notify_event_t *>(event);
      if (complete->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP)
      {
         m_last_present_msc = std::max(m_last_present_msc, complete->msc);
      }
      break;
   }
   default:
      break;
   }
   free(event);
}

bool swapchain::drain_present_events_locked()
{
   if (m_present_event == nullptr)
   {
      return false;
   }

   bool processed = false;
   xcb_generic_event_t *event = nullptr;
   while ((event = xcb_poll_for_special_event(m_connection, m_present_event)) != nullptr)
   {
      handle_present_event_locked(reinterpret_cast<xcb_present_generic_event_t *>(event));
      processed = true;
   }

   if (processed)
   {
      m_event_cv.notify_all();
   }
   return processed;
}

template <typename Predicate>
VkResult swapchain::wait_for_present_events(uint64_t abs_timeout, Predicate done)
{
   uint64_t poll_interval_ns = POLL_INTERVAL_MIN_NS;
   uint64_t last_activity_ns = monotonic_time_ns();

   std::unique_lock<std::mutex> lock(m_event_mutex);
   while (true)
   {
      if (drain_present_events_locked())
      {
         last_activity_ns = monotonic_time_ns();
         poll_interval_ns = POLL_INTERVAL_MIN_NS;
      }

      if (done())
      {
         return VK_SUCCESS;
      }

      if (xcb_connection_has_error(m_connection))
      {
         set_error_state(VK_ERROR_SURFACE_LOST_KHR);
         release_server_owned_images();
         return VK_ERROR_SURFACE_LOST_KHR;
      }

      if (error_has_occured())
      {
         return get_error_state();
      }

      const uint64_t now = monotonic_time_ns();
      if (abs_timeout != UINT64_MAX && now >= abs_timeout)
      {
         return VK_TIMEOUT;
      }

      if (now - last_activity_ns >= WINDOW_CHECK_INTERVAL_NS)
      {
         /* Round trip without holding the lock. */
         lock.unlock();
         const bool alive = window_exists(m_connection, m_window);
         lock.lock();
         if (!alive)
         {
            WSI_LOG_INFO("X window is lost!");
            set_error_state(VK_ERROR_OUT_OF_DATE_KHR);
            release_server_owned_images();
            return VK_ERROR_OUT_OF_DATE_KHR;
         }
         last_activity_ns = monotonic_time_ns();
         continue;
      }

      uint64_t wait_ns = poll_interval_ns;
      if (abs_timeout != UINT64_MAX)
      {
         wait_ns = std::min(wait_ns, abs_timeout - now);
      }

      if (m_event_poller_active)
      {
         /* Another thread is already blocked on the connection: wait for it to process the events. */
         m_event_cv.wait_for(lock, std::chrono::nanoseconds(wait_ns));
      }
      else
      {
         m_event_poller_active = true;
         lock.unlock();

         struct pollfd pfd = {};
         pfd.fd = xcb_get_file_descriptor(m_connection);
         pfd.events = POLLIN;
         const int timeout_ms = static_cast<int>(std::max<uint64_t>(1, (wait_ns + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC));
         const int ret = poll(&pfd, 1, timeout_ms);
         const int poll_errno = errno;

         lock.lock();
         m_event_poller_active = false;
         /* Let a waiting thread take over polling if this one is about to return. */
         m_event_cv.notify_all();

         if (ret < 0 && poll_errno != EINTR)
         {
            WSI_LOG_ERROR("poll() on the X connection failed (%d).", poll_errno);
            set_error_state(VK_ERROR_SURFACE_LOST_KHR);
            return VK_ERROR_SURFACE_LOST_KHR;
         }
      }

      /* Gradually increase the poll duration if nothing happens for a while: stutter is not the main concern
       * anymore at that point. A Present event is normally received once every refresh period. */
      poll_interval_ns = std::min(poll_interval_ns + poll_interval_ns / 2, POLL_INTERVAL_MAX_NS);
   }
}

void swapchain::present_image(const pending_present_request &pending_present)
{
   auto *image_data = reinterpret_cast<x11_image_data *>(m_swapchain_images[pending_present.image_index].data);
   const bool fifo = (m_present_mode == VK_PRESENT_MODE_FIFO_KHR);

   uint64_t target_msc = 0;
   {
      /* Release the images that became idle before presenting a new one. */
      std::lock_guard<std::mutex> lock(m_event_mutex);
      drain_present_events_locked();
      if (fifo)
      {
         target_msc = m_last_present_msc + 1;
      }
   }

   xshmfence_reset(image_data->shm_fence);

   uint32_t options = XCB_PRESENT_OPTION_NONE;
   if (m_present_mode == VK_PRESENT_MODE_MAILBOX_KHR && m_mailbox_async)
   {
      options |= XCB_PRESENT_OPTION_ASYNC;
   }
   if (m_force_copy)
   {
      options |= XCB_PRESENT_OPTION_COPY;
   }

   /* The request is not checked synchronously: xcb_request_check() would cost a full round trip per frame.
    * A failure (e.g. BadWindow) is detected through the window checks of wait_for_present_events(). */
   const xcb_void_cookie_t cookie = xcb_present_pixmap_checked(
      m_connection, m_window, image_data->pixmap, 0 /* serial */, XCB_NONE /* valid */, XCB_NONE /* update */,
      0 /* x_off */, 0 /* y_off */, XCB_NONE /* target_crtc */, XCB_NONE /* wait_fence */, image_data->sync_fence,
      options, target_msc, 0 /* divisor */, 0 /* remainder */, 0, nullptr);
   xcb_discard_reply(m_connection, cookie.sequence);
   xcb_flush(m_connection);

   if (xcb_connection_has_error(m_connection))
   {
      set_error_state(VK_ERROR_SURFACE_LOST_KHR);
      release_server_owned_images();
      return;
   }

   /* FIFO: wait for the present to complete before the next one, so that exactly one image is shown per vblank. */
   if (fifo)
   {
      const VkResult res =
         wait_for_present_events(UINT64_MAX, [this, target_msc]() { return m_last_present_msc >= target_msc; });
      if (res != VK_SUCCESS)
      {
         WSI_LOG_INFO("FIFO present wait ended with %d.", static_cast<int>(res));
      }
   }
}

void swapchain::destroy_image(swapchain_image &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);

   if (image.status != swapchain_image::INVALID)
   {
      if (image.image != VK_NULL_HANDLE)
      {
         m_device_data.disp.DestroyImage(m_device, image.image, get_allocation_callbacks());
         image.image = VK_NULL_HANDLE;
      }

      image.status = swapchain_image::INVALID;
   }

   image_status_lock.unlock();

   if (image.data != nullptr)
   {
      auto image_data = reinterpret_cast<x11_image_data *>(image.data);
      xcb_void_cookie_t cookie = {};

      if (image_data->shm_fence != nullptr)
      {
         cookie = xcb_sync_destroy_fence(m_connection, image_data->sync_fence);
         xcb_discard_reply(m_connection, cookie.sequence);
         xshmfence_unmap_shm(image_data->shm_fence);
         image_data->shm_fence = nullptr;
         image_data->sync_fence = XCB_NONE;
      }

      if (image_data->pixmap != XCB_NONE)
      {
         cookie = xcb_free_pixmap(m_connection, image_data->pixmap);
         xcb_discard_reply(m_connection, cookie.sequence);
         image_data->pixmap = XCB_NONE;
      }
      xcb_flush(m_connection);

      m_allocator.destroy(1, image_data);
      image.data = nullptr;
   }
}

bool swapchain::free_image_found()
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   for (const auto &img : m_swapchain_images)
   {
      if (img.status == swapchain_image::FREE)
      {
         return true;
      }
   }
   return false;
}

VkResult swapchain::get_free_buffer(uint64_t *timeout)
{
   /* Idle images are only returned through IdleNotify events, so they must be processed here in every present
    * mode. Previously this was only done for MAILBOX: in FIFO an acquire could stall until the page flip thread
    * happened to wake up (up to 100 ms). */
   const VkResult res = wait_for_present_events(absolute_timeout_ns(*timeout), [this]() { return free_image_found(); });

   if (res == VK_TIMEOUT)
   {
      /* The previous code compared the pointer, not the value, with 0 and never returned VK_NOT_READY. */
      return (*timeout == 0) ? VK_NOT_READY : VK_TIMEOUT;
   }
   return res;
}

void swapchain::update_pending_image_status()
{
   /* Never blocks: if another thread is processing events it will release the images. */
   std::unique_lock<std::mutex> lock(m_event_mutex, std::try_to_lock);
   if (lock.owns_lock())
   {
      drain_present_events_locked();
   }
}

VkResult swapchain::image_set_present_payload(swapchain_image &image, VkQueue queue,
                                              const queue_submit_semaphores &semaphores, const void *submission_pnext)
{
   auto image_data = reinterpret_cast<x11_image_data *>(image.data);
   return image_data->present_fence.set_payload(queue, semaphores, submission_pnext);
}

VkResult swapchain::image_wait_present(swapchain_image &image, uint64_t timeout)
{
   auto image_data = reinterpret_cast<x11_image_data *>(image.data);
   return image_data->present_fence.wait_payload(timeout);
}

VkResult swapchain::bind_swapchain_image(VkDevice &device, const VkBindImageMemoryInfo *bind_image_mem_info,
                                         const VkBindImageMemorySwapchainInfoKHR *bind_sc_info)
{
   UNUSED(device);
   const wsi::swapchain_image &swapchain_image = m_swapchain_images[bind_sc_info->imageIndex];
   auto image_data = reinterpret_cast<x11_image_data *>(swapchain_image.data);
   return image_data->external_mem.bind_swapchain_image_memory(bind_image_mem_info->image);
}

} // namespace xcb
} // namespace wsi
