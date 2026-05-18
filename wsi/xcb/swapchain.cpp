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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <climits>
#include <functional>
#include <array>

#include "util/drm/drm_utils.hpp"
#include "util/log.hpp"
#include "util/helpers.hpp"
#include "util/macros.hpp"
#include "util/format_modifiers.hpp"

#include "xcb_helpers.hpp"

#define MIN2( A, B )   ( (A)<(B) ? (A) : (B) )
#define MAX2( A, B )   ( (A)>(B) ? (A) : (B) )

namespace wsi
{
namespace xcb
{

const VkImageAspectFlagBits plane_flag_bits[MAX_PLANES] = {
   VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
   VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
   VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
   VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
};

swapchain::swapchain(layer::device_private_data &dev_data, const VkAllocationCallbacks *pAllocator,
                     surface &wsi_surface)
   : swapchain_base(dev_data, pAllocator)
   , m_connection(wsi_surface.get_xcb_connection())
   , m_window(wsi_surface.get_xcb_window())
   , m_wsi_surface(&wsi_surface)
   , m_wsi_allocator(nullptr)
   , m_image_creation_parameters({}, m_allocator, {}, {})
   , m_last_present_msc(0)
   , m_window_width(0)
   , m_window_height(0)

{
   m_image_create_info.format = VK_FORMAT_UNDEFINED;
}

swapchain::~swapchain()
{
   teardown();

   xcb_unregister_for_special_event(m_connection, m_present_event);
   xcb_void_cookie_t cookie = xcb_present_select_input_checked(m_connection, m_event_id,
                                             m_window,
                                             XCB_PRESENT_EVENT_MASK_NO_EVENT);
   xcb_discard_reply(m_connection, cookie.sequence);


   if (m_wsi_allocator != nullptr)
   {
      wsialloc_delete(m_wsi_allocator);
   }
   m_wsi_allocator = nullptr;
}

VkResult swapchain::init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                                  bool &use_presentation_thread)
{
   m_window_width = swapchain_create_info->imageExtent.width;
   m_window_height = swapchain_create_info->imageExtent.height;
   WSI_LOG_INFO("Swapchain min image count is %d.", swapchain_create_info->minImageCount);

   // Select event
   m_event_id = xcb_generate_id(m_connection);

   xcb_present_select_input(m_connection, m_event_id, m_window,
                            XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY |
                            XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY  |
                            XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);

   m_present_event = xcb_register_for_special_xge(m_connection, &xcb_present_id, m_event_id, NULL);

   WSIALLOC_ASSERT_VERSION();
   if (wsialloc_new(&m_wsi_allocator) != WSIALLOC_ERROR_NONE)
   {
      WSI_LOG_ERROR("Failed to create wsi allocator.");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   set_adaptive_sync_property(m_connection, m_window, false);

   use_presentation_thread = true;
   const char* env_var = getenv("DISABLE_XCB_PRESENT_THREAD");
   if (env_var)
   {
      use_presentation_thread = false;
   }

   return VK_SUCCESS;
}

VkResult swapchain::get_fd_mem_type_index(int fd, uint32_t &mem_idx)
{
   VkMemoryFdPropertiesKHR mem_props = {};
   mem_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;

   VkResult result = m_device_data.disp.GetMemoryFdPropertiesKHR(
      m_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &mem_props);
   if (result != VK_SUCCESS)
   {
      WSI_LOG_ERROR("Error querying Fd properties.");
      return result;
   }

   for (mem_idx = 0; mem_idx < VK_MAX_MEMORY_TYPES; mem_idx++)
   {
      if (mem_props.memoryTypeBits & (1 << mem_idx))
      {
         break;
      }
   }

   assert(mem_idx < VK_MAX_MEMORY_TYPES);

   return VK_SUCCESS;
}

static uint32_t get_same_fd_index(int fd, int const *fds)
{
   uint32_t index = 0;
   while (fd != fds[index])
   {
      index++;
   }

   return index;
}

VkResult swapchain::get_surface_compatible_formats(const VkImageCreateInfo &info,
                                                   util::vector<wsialloc_format> &importable_formats,
                                                   util::vector<uint64_t> &exportable_modifers,
                                                   util::vector<VkDrmFormatModifierPropertiesEXT> &drm_format_props)
{
   TRY_LOG(util::get_drm_format_properties(m_device_data.physical_device, info.format, drm_format_props),
           "Failed to get format properties");

   for (const auto &prop : drm_format_props)
   {
      bool is_supported = false;
      drm_format_pair drm_format{ util::drm::vk_to_drm_format(info.format), prop.drmFormatModifier };

      for (const auto &format : m_wsi_surface->get_formats())
      {
         if (format.fourcc == drm_format.fourcc && format.modifier == drm_format.modifier)
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

      VkResult result = VK_SUCCESS;
      {
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
         result = m_device_data.instance_data.disp.GetPhysicalDeviceImageFormatProperties2KHR(
            m_device_data.physical_device, &image_info, &format_props);
      }
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
         if (!exportable_modifers.try_push_back(drm_format.modifier))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }

      if (external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR)
      {
         uint64_t flags =
            (prop.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_DISJOINT_BIT) ? 0 : WSIALLOC_FORMAT_NON_DISJOINT;
         wsialloc_format import_format{ drm_format.fourcc, drm_format.modifier, flags };
         if (!importable_formats.try_push_back(import_format))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }
   }

   return VK_SUCCESS;
}

VkResult swapchain::allocate_wsialloc(VkImageCreateInfo             &image_create_info,
                                      x11_image_data                *image_data,
                                      util::vector<wsialloc_format> &importable_formats,
                                      wsialloc_format               *allocated_format,
                                      bool                          avoid_allocation)
{
   bool is_protected_memory = (image_create_info.flags & VK_IMAGE_CREATE_PROTECTED_BIT) != 0;
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

   wsialloc_allocate_info alloc_info = { importable_formats.data(), static_cast<unsigned>(importable_formats.size()),
                                         image_create_info.extent.width, image_create_info.extent.height,
                                         allocation_flags };

   wsialloc_allocate_result alloc_result = { 0 };
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

   uint32_t num_planes = util::drm::drm_fourcc_format_get_num_planes(alloc_result.format.fourcc);

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

static VkResult fill_image_create_info(VkImageCreateInfo                             &image_create_info,
                                       util::vector<VkSubresourceLayout>             &image_plane_layouts,
                                       VkImageDrmFormatModifierExplicitCreateInfoEXT &drm_mod_info,
                                       VkExternalMemoryImageCreateInfoKHR            &external_info,
                                        x11_image_data                               &image_data,
                                       uint64_t                                       modifier)
{
   TRY_LOG_CALL(image_data.external_mem.fill_image_plane_layouts(image_plane_layouts));

   if (image_data.external_mem.is_disjoint())
   {
      image_create_info.flags |= VK_IMAGE_CREATE_DISJOINT_BIT;
   }

   image_data.external_mem.fill_drm_mod_info(image_create_info.pNext, drm_mod_info, image_plane_layouts, modifier);
   image_data.external_mem.fill_external_info(external_info, &drm_mod_info);
   image_create_info.pNext  = &external_info;
   image_create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
   return VK_SUCCESS;
}

VkResult swapchain::allocate_image(VkImageCreateInfo &image_create_info,
                                   x11_image_data    *image_data)
{
   util::vector<wsialloc_format> importable_formats(util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
   auto &m_allocated_format = m_image_creation_parameters.m_allocated_format;

   if (!importable_formats.try_push_back(m_allocated_format))
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   TRY_LOG_CALL(allocate_wsialloc(m_image_create_info, image_data, importable_formats, &m_allocated_format, false));

   return VK_SUCCESS;
}

VkResult swapchain::x11_image_init(VkImageCreateInfo         image_create_info,
                                   x11_image_data            *image_data,
                                   image_creation_parameters &creation_parameters)
{
   xcb_void_cookie_t cookie      = {};
   std::array<int, 4> buffer_fds = image_data->external_mem.get_buffer_fds();

   image_data->pixmap = xcb_generate_id(m_connection);

   if (creation_parameters.m_allocated_format.modifier != DRM_FORMAT_MOD_INVALID)
   {
      int fds[4] = { -1, -1, -1, -1 };
      for (int i = 0; i < image_data->external_mem.get_num_planes(); i++)
      {
         fds[i] = dup(buffer_fds[0]);

         if (fds[i] == -1) {
            for (int j = 0; j < i; j++)
               close(fds[j]);

            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }

      cookie =
         xcb_dri3_pixmap_from_buffers_checked(m_connection,
                                              image_data->pixmap,
                                              m_window,
                                              image_data->external_mem.get_num_planes(),
                                              image_create_info.extent.width,
                                              image_create_info.extent.height,
                                              image_data->external_mem.get_strides()[0],
                                              image_data->external_mem.get_offsets()[0],
                                              image_data->external_mem.get_strides()[1],
                                              image_data->external_mem.get_offsets()[1],
                                              image_data->external_mem.get_strides()[2],
                                              image_data->external_mem.get_offsets()[2],
                                              image_data->external_mem.get_strides()[3],
                                              image_data->external_mem.get_offsets()[3],
                                              m_wsi_surface->get_bit_depth(),
                                              m_wsi_surface->get_bpp(),
                                              creation_parameters.m_allocated_format.modifier,
                                              fds);
   }
   else
   {
      int fd = dup(buffer_fds[0]);
      if (fd == -1)
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      uint32_t size   = image_create_info.extent.height * image_data->external_mem.get_strides()[0];
               cookie = xcb_dri3_pixmap_from_buffer_checked(m_connection,
                                                            image_data->pixmap,
                                                            m_window,
                                                            size,
                                                            image_create_info.extent.width,
                                                            image_create_info.extent.height,
                                                            image_data->external_mem.get_strides()[0],
                                                            m_wsi_surface->get_bit_depth(),
                                                            m_wsi_surface->get_bpp(),
                                                            fd);
   }

   xcb_flush(m_connection);
   xcb_discard_reply(m_connection, cookie.sequence);

   xcb_generic_error_t *error   = xcb_request_check(m_connection, cookie);
   VkResult            result   = VK_SUCCESS;
   int                 fence_fd = -1;

   if (error)
   {
      WSI_LOG_ERROR("Could not create pixmap from buffer with error %d \n", error->error_code);
      free(error);
      result = VK_ERROR_UNKNOWN;
   }

   if (result == VK_SUCCESS)
   {
      fence_fd = xshmfence_alloc_shm();
      result   = fence_fd < 0 ? VK_ERROR_UNKNOWN : VK_SUCCESS;
   }

   if (result == VK_SUCCESS)
   {
      image_data->shm_fence = xshmfence_map_shm(fence_fd);
      result                = image_data->shm_fence == NULL ? VK_ERROR_UNKNOWN : VK_SUCCESS;
   }
   else if (fence_fd >= 0)
   {
      close(fence_fd);
   }

   if (result == VK_SUCCESS)
   {
      image_data->sync_fence = xcb_generate_id(m_connection);
      xcb_dri3_fence_from_fd(m_connection,
                             image_data->pixmap,
                             image_data->sync_fence,
                             false,
                             fence_fd);
      xshmfence_trigger(image_data->shm_fence);
   }
   else
   {
      cookie = xcb_free_pixmap(m_connection, image_data->pixmap);
      xcb_discard_reply(m_connection, cookie.sequence);
   }

   return result;
}

VkResult swapchain::allocate_and_bind_swapchain_image(VkImageCreateInfo  image_create_info,
                                                    swapchain_image   &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   image.status = swapchain_image::FREE;

   assert(image.data != nullptr);
   auto image_data = static_cast<x11_image_data *>(image.data);
   TRY_LOG(allocate_image(image_create_info, image_data),
          "Failed to allocate image");

   image_status_lock.unlock();

   TRY_LOG(x11_image_init(image_create_info, image_data, m_image_creation_parameters),
           "Failed to init x11 image.");

   TRY_LOG(image_data->external_mem.import_memory_and_bind_swapchain_image(image.image),
           "Failed to import memory and bind swapchain image");

   /* Initialize presentation fence. */
   auto present_fence = sync_fd_fence_sync::create(m_device_data);
   if (!present_fence.has_value())
   {
      destroy_image(image);
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

      wsialloc_format allocated_format = { 0 };
      TRY_LOG_CALL(allocate_wsialloc(image_create_info, image_data, importable_formats, &allocated_format, true));

      for (auto &prop : drm_format_props)
      {
         if (prop.drmFormatModifier == allocated_format.modifier)
         {
            image_data->external_mem.set_num_memories(prop.drmFormatModifierPlaneCount);
         }
      }

      TRY_LOG_CALL(fill_image_create_info(image_create_info,
                                          m_image_creation_parameters.m_image_layout,
                                          m_image_creation_parameters.m_drm_mod_info,
                                          m_image_creation_parameters.m_external_info,
                                          *image_data,
                                          allocated_format.modifier));

      m_image_create_info = image_create_info;
      m_image_creation_parameters.m_allocated_format = allocated_format;
   }

   return m_device_data.disp.CreateImage(m_device, &m_image_create_info, get_allocation_callbacks(), &image.image);
}

void swapchain::present_image(const pending_present_request &pending_present)
{
   int res;

   // Poll the present event to unpresent the idle image.
   xcb_present_generic_event_t *poll_event;
   while (poll_event = (xcb_present_generic_event_t*)xcb_poll_for_special_event(m_connection, m_present_event))
   {
      handle_dri3_present_event(poll_event);
   }

   x11_image_data *image_data = reinterpret_cast<x11_image_data *>(m_swapchain_images[pending_present.image_index].data);

   xshmfence_reset(image_data->shm_fence);

   uint64_t target_msc = 0;
   if (m_present_mode == VK_PRESENT_MODE_FIFO_KHR)
   {
      target_msc = m_last_present_msc + 1;
   }
   uint64_t remainder = 0;
   uint64_t divisor = 0;
   uint32_t options = XCB_PRESENT_OPTION_NONE;

   if (m_present_mode == VK_PRESENT_MODE_MAILBOX_KHR)
   {
      options |= XCB_PRESENT_OPTION_ASYNC;
   }
   const char* env_var = getenv("DISABLE_XCB_FLIP");
   if(env_var)
   {
      options |= XCB_PRESENT_OPTION_COPY;
   }

   xcb_void_cookie_t cookie =
      xcb_present_pixmap_checked(m_connection,
                         m_window,
                         image_data->pixmap,
                         0,
                         0,                                    /* valid */
                         0,                                    /* update */
                         0,                                    /* x_off */
                         0,                                    /* y_off */
                         XCB_NONE,                             /* target_crtc */
                         XCB_NONE,
                         image_data->sync_fence,
                         options,
                         target_msc,
                         divisor,
                         remainder,
                         0,
                         NULL);

   xcb_generic_error_t *error;
   if (error = xcb_request_check(m_connection, cookie))
   {
      WSI_LOG_ERROR("Could not present pixmap with error %d \n", error->error_code);
      set_error_state(VK_ERROR_OUT_OF_DATE_KHR);
      free(error);
      return;
   }

   xcb_discard_reply(m_connection, cookie.sequence);

   /* We flush the request */
   xcb_flush (m_connection);

   // FIFO needs to wait for the frame to complete before starting a new loop.
   if (m_present_mode == VK_PRESENT_MODE_FIFO_KHR)
   {

      while (m_last_present_msc < target_msc)
      {
         // Check if the X window still exists at this point.
         // This is because, in the current Zink solution, the X window is always destroyed before the Vulkan
         // swapchain.
         // This prevents getting stuck when calling xcb_wait_for_special_event and has less CPU overhead.
         xcb_get_window_attributes_cookie_t att_cookie = xcb_get_window_attributes(m_connection, m_window);
         xcb_get_window_attributes_reply_t *reply = xcb_get_window_attributes_reply(m_connection, att_cookie, NULL);
         if (!reply)
         {
            // Free all images since the X window no longer exists.
            for (auto i = 0; i < m_swapchain_images.size(); i++)
            {
               if ((m_swapchain_images[i].status != swapchain_image::INVALID) &&
                   (m_swapchain_images[i].status != swapchain_image::FREE))
               {
                     unpresent_image(i);
               }
            }

            set_error_state(VK_ERROR_OUT_OF_DATE_KHR);
            WSI_LOG_INFO("X window is lost!");
            return;
         }
         free(reply);

         xcb_present_generic_event_t *event =
            (xcb_present_generic_event_t *)xcb_wait_for_special_event(m_connection, m_present_event);

         if (!event)
         {
            WSI_LOG_INFO("Waiting for special event failed!");
            set_error_state(VK_ERROR_OUT_OF_DATE_KHR);
            return;
         }
         handle_dri3_present_event(event);
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
      auto              image_data = reinterpret_cast<x11_image_data *>(image.data);
      xcb_void_cookie_t cookie     = {};

      if (image_data->shm_fence != nullptr)
      {
         cookie = xcb_sync_destroy_fence(m_connection, image_data->sync_fence);
         xcb_discard_reply(m_connection, cookie.sequence);
         xshmfence_unmap_shm(image_data->shm_fence);
         image_data->shm_fence = nullptr;
      }

      if (image_data->pixmap != 0)
      {
         cookie = xcb_free_pixmap(m_connection, image_data->pixmap);
         xcb_discard_reply(m_connection, cookie.sequence);
         image_data->pixmap = 0;
      }

      m_allocator.destroy(1, image_data);
      image.data = nullptr;
   }
}

bool swapchain::free_image_found()
{
   if (m_present_mode == VK_PRESENT_MODE_MAILBOX_KHR)
   {
      poll_present_event_for_pending_image();
   }

   for (auto &img : m_swapchain_images)
   {
      if (img.status == swapchain_image::FREE)
      {
         return true;
      }
   }
   return false;
}

static int64_t os_time_get_nano(void)
{
   struct timespec tv;
   clock_gettime(CLOCK_MONOTONIC, &tv);
   return tv.tv_nsec + tv.tv_sec*INT64_C(1000000000);
}

static int64_t os_time_get_absolute_timeout(uint64_t timeout)
{
   int64_t time, abs_timeout;

   time = os_time_get_nano();

   abs_timeout = time + (int64_t)timeout;

   return abs_timeout;
}

VkResult swapchain::get_free_buffer(uint64_t *timeout)
{
   uint64_t abs_timeout;
   VkResult ret = VK_SUCCESS;
   if (*timeout >= UINT64_MAX)
   {
      abs_timeout = UINT64_MAX;
   }
   else
   {
      abs_timeout = os_time_get_absolute_timeout(*timeout);
   }

   struct pollfd pfd = {};
   uint64_t rel_timeout;
   /* Start out with 1 ms intervals since that's what poll() supports. */
   uint64_t poll_busywait_ns = 1000 * 1000;

   /* Currently, the idle image will be unpresented in the present_image() function.
    * Since the presentation thread is enabled by default, the idle image can always
    * be unpresented by that thread.
    * If disable presentation thread, the idle image may not get a chance to unpresent
    * in some scenarios.
    *
    * The loop exits with a free image or timeout.
    */
   while (!free_image_found())
   {
      /* If a non-special event happens, the fd will still
       * poll. So recalculate the timeout now just in case.
       */
      struct timespec tv;
      uint64_t current_time = os_time_get_nano();
      if (abs_timeout > current_time)
         rel_timeout = MIN2(poll_busywait_ns, abs_timeout - current_time);
      else
         rel_timeout = 0;

      if (rel_timeout)
      {
         pfd.fd = xcb_get_file_descriptor(m_connection);
         pfd.events = POLLIN;

         int err = poll(&pfd, 1, MAX2(rel_timeout / 1000 / 1000, 1u));
         if (err == -1)
         {
            WSI_LOG_INFO("Poll failed!");
            return VK_ERROR_OUT_OF_DATE_KHR;
         }

         /* Gradually increase the poll duration if it takes a very long time to receive a poll event,
          * since at that point, stutter isn't really the main concern anymore.
          * We generally expect a special event to be received once every refresh duration. */
         poll_busywait_ns += poll_busywait_ns / 2;
         poll_busywait_ns = MIN2(10ull * 1000ull * 1000ull, poll_busywait_ns);
      }

      if (rel_timeout == 0)
      {
         ret = VK_TIMEOUT;
         break;
      }
   }

   if (ret == VK_TIMEOUT)
   {
      ret = timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
   }

   return ret;
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
   const wsi::swapchain_image &swapchain_image = m_swapchain_images[bind_sc_info->imageIndex];
   auto image_data = reinterpret_cast<x11_image_data *>(swapchain_image.data);
   return image_data->external_mem.bind_swapchain_image_memory(bind_image_mem_info->image);
}


/* XXX this belongs in presentproto */
#ifndef PresentWindowDestroyed
#define PresentWindowDestroyed (1 << 0)
#endif
/**
 * Process an X11 Present event.
 */
void swapchain::handle_dri3_present_event(xcb_present_generic_event_t *event)
{
   switch (event->evtype)
   {
   case XCB_PRESENT_CONFIGURE_NOTIFY:
   {
      xcb_present_configure_notify_event_t *config = (xcb_present_configure_notify_event_t *)event;
      if (config->pixmap_flags & PresentWindowDestroyed)
         set_error_state(VK_ERROR_SURFACE_LOST_KHR);

      if (config->width != m_window_width ||
          config->height != m_window_height)
      {
         WSI_LOG_INFO("Window size changed!");
         set_error_state(VK_SUBOPTIMAL_KHR);
      }

      break;
   }
   case XCB_PRESENT_EVENT_IDLE_NOTIFY:
   {
      xcb_present_idle_notify_event_t *idle = (xcb_present_idle_notify_event_t *)event;
      for (auto i = 0; i < m_swapchain_images.size(); i++)
      {
         if (m_swapchain_images[i].status != swapchain_image::INVALID)
         {
            auto image_data = reinterpret_cast<x11_image_data *>(m_swapchain_images[i].data);
            if (image_data->pixmap == idle->pixmap)
            {
               if (m_swapchain_images[i].status != swapchain_image::FREE)
               {
                  // Make sure X has released that image before the driver unpresents it.
                  xshmfence_await(image_data->shm_fence);
                  unpresent_image(i);
                  break;
               }
            }
         }
      }
      break;
   }
   case XCB_PRESENT_EVENT_COMPLETE_NOTIFY:
   {
      xcb_present_complete_notify_event_t *complete = (xcb_present_complete_notify_event_t *)event;

      if (complete->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP)
      {
         m_last_present_msc = complete->msc;
      }

      break;
   }
   default:
      break;
   }
   free(event);
}

void swapchain::update_pending_image_status()
{
   if (m_present_mode == VK_PRESENT_MODE_FIFO_KHR)
   {
      poll_present_event_for_pending_image();
   }
}

void swapchain::poll_present_event_for_pending_image()
{
   bool has_pending_image = false;
   for (auto &img : m_swapchain_images)
   {
      if (img.status == swapchain_image::PENDING)
      {
         has_pending_image = true;
         break;
      }
   }
   // Polling X present event before checking the image status.
   // This ensures that the driver can receive the free image from X promptly.
   if (has_pending_image)
   {
      xcb_present_generic_event_t *poll_event;
      while (poll_event = (xcb_present_generic_event_t *)xcb_poll_for_special_event(m_connection, m_present_event))
      {
         handle_dri3_present_event(poll_event);
      }
   }
}

} // namespace xcb
} // namespace wsi
