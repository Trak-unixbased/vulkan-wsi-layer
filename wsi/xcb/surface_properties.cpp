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

#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shm.h>

extern "C" {
#include <X11/xshmfence.h>
}

#include "xcb_helpers.hpp"

#include <cassert>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstring>
#include "surface_properties.hpp"
#include "surface.hpp"
#include "layer/private_data.hpp"

#include "util/drm/drm_utils.hpp"
#include "util/log.hpp"
#include "util/macros.hpp"
#include "util/helpers.hpp"

#define NELEMS(x) (sizeof(x) / sizeof(x[0]))

namespace wsi
{
namespace xcb
{

void surface_properties::populate_present_mode_compatibilities()
{
   std::array<present_mode_compatibility, 2> compatible_present_modes_list = {
      present_mode_compatibility{ VK_PRESENT_MODE_FIFO_KHR, 1, { VK_PRESENT_MODE_FIFO_KHR } },
      present_mode_compatibility{ VK_PRESENT_MODE_MAILBOX_KHR, 1, { VK_PRESENT_MODE_MAILBOX_KHR } }
   };
   m_compatible_present_modes = compatible_present_modes<2>(compatible_present_modes_list);
}

surface_properties::surface_properties(surface *wsi_surface, const util::allocator &allocator)
   : specific_surface(wsi_surface)
   , supported_formats(allocator)
   , m_supported_modes({ VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR })
{
   populate_present_mode_compatibilities();
}

surface_properties::surface_properties()
   : specific_surface(nullptr)
   , supported_formats(util::allocator::get_generic())
{
}

surface_properties &surface_properties::get_instance()
{

   static surface_properties instance;
   return instance;

}

VkResult surface_properties::get_surface_capabilities(VkPhysicalDevice physical_device,
                                                      VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
   xcb_connection_t          * conn       = specific_surface->get_xcb_connection();
   xcb_window_t              window       = specific_surface->get_xcb_window();
   xcb_get_geometry_cookie_t geom_cookie  = {};
   unsigned                  visual_depth = 0;

   /* Image count limits */
   get_surface_capabilities_common(physical_device, pSurfaceCapabilities);
   // 4 images have a better performance in mailbox mode.
   pSurfaceCapabilities->minImageCount = 4;

   const char* env_var = getenv("XCB_SWAPCHAIN_MIN_IMAGE_COUNT");
   if(env_var)
   {
      long min_image_count = strtol(env_var, nullptr, 0);
      if (min_image_count >= 0)
      {
         pSurfaceCapabilities->minImageCount = min_image_count;
      }
   }

   geom_cookie = xcb_get_geometry(conn, window);

   xcb_visualtype_t *visual =
      get_visualtype_for_window(conn, window, &visual_depth);

   if (!visual)
      return VK_ERROR_SURFACE_LOST_KHR;

   xcb_get_geometry_reply_t *geometry =
      xcb_get_geometry_reply(conn, geom_cookie, NULL);

   if (geometry == NULL)
      return VK_ERROR_SURFACE_LOST_KHR;
 
   pSurfaceCapabilities->currentExtent.width = geometry->width;
   pSurfaceCapabilities->currentExtent.height = geometry->height;

   free(geometry);

   if (visual_has_alpha(visual, visual_depth)) {
      pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR |
                                                      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
   } else {
      pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR |
                                                      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   }

   return VK_SUCCESS;
}

VkResult surface_properties::get_surface_capabilities(VkPhysicalDevice physical_device,
                                                      const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                                      VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   TRY(check_surface_present_mode_query_is_supported(pSurfaceInfo, m_supported_modes));

   /* Image count limits */
   get_surface_capabilities(physical_device, &pSurfaceCapabilities->surfaceCapabilities);

   m_compatible_present_modes.get_surface_present_mode_compatibility_common(pSurfaceInfo, pSurfaceCapabilities);

   auto surface_scaling_capabilities = util::find_extension<VkSurfacePresentScalingCapabilitiesEXT>(
      VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT, pSurfaceCapabilities);
   if (surface_scaling_capabilities != nullptr)
   {
      get_surface_present_scaling_and_gravity(surface_scaling_capabilities);
      surface_scaling_capabilities->minScaledImageExtent = pSurfaceCapabilities->surfaceCapabilities.minImageExtent;
      surface_scaling_capabilities->maxScaledImageExtent = pSurfaceCapabilities->surfaceCapabilities.maxImageExtent;
   }

   return VK_SUCCESS;
}

static VkResult surface_format_properties_add_modifier_support(VkPhysicalDevice phys_dev,
                                                               surface_format_properties &format_props,
#if WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN
                                                               const drm_format_pair &drm_format,
                                                               bool add_compression = false)
#else
                                                               const drm_format_pair &drm_format)
#endif
{
   VkPhysicalDeviceExternalImageFormatInfoKHR external_info = {};
   external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR;
   external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

   VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_mod_info = {};
   drm_mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
   drm_mod_info.pNext = &external_info;
   drm_mod_info.drmFormatModifier = drm_format.modifier;
   drm_mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

   VkPhysicalDeviceImageFormatInfo2KHR image_info = {};
   image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR;
   image_info.pNext = &drm_mod_info;
   image_info.format = format_props.m_surface_format.format;
   image_info.type = VK_IMAGE_TYPE_2D;
   image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
   image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

#if WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN
   if (add_compression)
   {
      return format_props.add_device_compression_support(phys_dev, image_info);
   }
#endif

   return format_props.check_device_support(phys_dev, image_info);
}

static VkResult surface_format_properties_map_add(VkPhysicalDevice phys_dev, surface_format_properties_map &format_map,
                                                  VkFormat format, const drm_format_pair &drm_format)
{
   surface_format_properties format_props{ format };
   VkResult res = surface_format_properties_add_modifier_support(phys_dev, format_props, drm_format);
   if (res == VK_SUCCESS)
   {
      auto it = format_map.try_insert(std::make_pair(format, format_props));
      if (!it.has_value())
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   if (res == VK_ERROR_FORMAT_NOT_SUPPORTED)
   {
      return VK_SUCCESS;
   }

   return res;
}

static VkResult surface_format_properties_map_init(VkPhysicalDevice phys_dev, surface_format_properties_map &format_map,
                                                   const util::vector<drm_format_pair> &drm_format_list)
{
   for (const auto &drm_format : drm_format_list)
   {
      const VkFormat vk_format = util::drm::drm_to_vk_format(drm_format.fourcc);
      if (vk_format != VK_FORMAT_UNDEFINED && format_map.find(vk_format) == format_map.end())
      {
         TRY(surface_format_properties_map_add(phys_dev, format_map, vk_format, drm_format));
      }
      const VkFormat srgb_vk_format = util::drm::drm_to_vk_srgb_format(drm_format.fourcc);
      if (srgb_vk_format != VK_FORMAT_UNDEFINED && format_map.find(srgb_vk_format) == format_map.end())
      {
         TRY(surface_format_properties_map_add(phys_dev, format_map, srgb_vk_format, drm_format));
      }
   }

   return VK_SUCCESS;
}

#if WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN
static VkResult surface_format_properties_map_add_compression(VkPhysicalDevice phys_dev,
                                                              surface_format_properties_map &format_map,
                                                              const util::vector<drm_format_pair> &drm_format_list)
{
   for (const auto &drm_format : drm_format_list)
   {
      const VkFormat vk_format = util::drm::drm_to_vk_format(drm_format.fourcc);
      if (vk_format != VK_FORMAT_UNDEFINED)
      {
         auto entry = format_map.find(vk_format);
         if (entry != format_map.end())
         {
            TRY(surface_format_properties_add_modifier_support(phys_dev, entry->second, drm_format, true));
         }
      }
      const VkFormat srgb_vk_format = util::drm::drm_to_vk_srgb_format(drm_format.fourcc);
      if (srgb_vk_format != VK_FORMAT_UNDEFINED)
      {
         auto entry = format_map.find(srgb_vk_format);
         if (entry != format_map.end())
         {
            TRY(surface_format_properties_add_modifier_support(phys_dev, entry->second, drm_format, true));
         }
      }
   }
   return VK_SUCCESS;
}
#endif

VkResult surface_properties::get_surface_formats(VkPhysicalDevice physical_device, uint32_t *surfaceFormatCount,
                                                 VkSurfaceFormatKHR *surfaceFormats,
                                                 VkSurfaceFormat2KHR *extended_surface_formats)
{
   assert(specific_surface);
   if (!supported_formats.size())
   {
      TRY(surface_format_properties_map_init(physical_device, supported_formats, specific_surface->get_formats()));
#if WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN
      if (layer::instance_private_data::get(physical_device).has_image_compression_support(physical_device))
      {
         TRY(surface_format_properties_map_add_compression(physical_device, supported_formats,
                                                           specific_surface->get_formats()));
      }
#endif
   }

   return surface_properties_formats_helper(supported_formats.begin(), supported_formats.end(), surfaceFormatCount,
                                            surfaceFormats, extended_surface_formats);
}

VkResult surface_properties::get_surface_present_modes(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                                       uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes)
{
   return get_surface_present_modes_common(pPresentModeCount, pPresentModes, m_supported_modes);
}


VkResult surface_properties::get_required_device_extensions(util::extension_list &extension_list)
{
   const std::array required_device_extensions{
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
      VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
      VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
      VK_KHR_MAINTENANCE1_EXTENSION_NAME,
      VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
      VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
   };

   return extension_list.add(required_device_extensions.data(), required_device_extensions.size());
}

VkResult surface_properties::get_required_instance_extensions(util::extension_list &extension_list)
{
   const std::array required_instance_extensions{
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
      VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
   };
   return extension_list.add(required_instance_extensions.data(), required_instance_extensions.size());
}

struct required_properties
{
   bool dmabuf;
   bool explicit_sync;
};


VWL_VKAPI_CALL(VkBool32)
GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physical_device,
                                           uint32_t queue_index,
                                           xcb_connection_t *connection,
                                           xcb_visualid_t visual_id)
{
   bool dev_supports_sync =
      sync_fd_fence_sync::is_supported(layer::instance_private_data::get(physical_device), physical_device);
   if (!dev_supports_sync)
   {
      return VK_FALSE;
   }
   struct connection_caps caps = {0};

   connection_get_caps(connection, &caps);

   if (caps.has_dri3 == false)
   {
      WSI_LOG_ERROR("vulkan: No DRI3 support detected - required for presentation\n"
                    "Note: you can probably enable DRI3 in your Xorg config\n");
      return VK_FALSE;
   }

   if (caps.has_present == false)
   {
      WSI_LOG_ERROR("vulkan: No Present support detected - required for presentation\n"
                    "Note: you can probably enable Present in your Xorg config\n");
      return VK_FALSE;
   }

   if ((!visual_supported(connection_get_visualtype(connection, visual_id))))
   {
      return VK_FALSE;
   }

   return VK_TRUE;
}

VWL_VKAPI_CALL(VkBool32)
GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physical_device,
                                           uint32_t queue_index,
                                           Display *dpy,
                                           VisualID visualID)
{
   return GetPhysicalDeviceXcbPresentationSupportKHR(physical_device,
                                                     queue_index,
                                                     XGetXCBConnection(dpy),
                                                     visualID);
}

VWL_VKAPI_CALL(VkResult)
CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) VWL_API_POST
{
   auto &instance_data = layer::instance_private_data::get(instance);
   util::allocator allocator{ instance_data.get_allocator(), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, pAllocator };
   auto wsi_surface = surface::make_surface(allocator, pCreateInfo->connection, pCreateInfo->window);
   if (wsi_surface == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   VkResult res = instance_data.disp.CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
   if (res == VK_SUCCESS)
   {
      auto surface_base = util::unique_ptr<wsi::surface>(std::move(wsi_surface));
      res = instance_data.add_surface(*pSurface, surface_base);
      if (res != VK_SUCCESS)
      {
         instance_data.disp.DestroySurfaceKHR(instance, *pSurface, pAllocator);
      }
   }
   return res;
}

VWL_VKAPI_CALL(VkResult)
CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) VWL_API_POST
{
   auto &instance_data = layer::instance_private_data::get(instance);
   util::allocator allocator{ instance_data.get_allocator(), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, pAllocator };

   auto wsi_surface = surface::make_surface(allocator, XGetXCBConnection(pCreateInfo->dpy), pCreateInfo->window);
   if (wsi_surface == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   VkResult res = instance_data.disp.CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
   if (res == VK_SUCCESS)
   {
      auto surface_base = util::unique_ptr<wsi::surface>(std::move(wsi_surface));
      res = instance_data.add_surface(*pSurface, surface_base);
      if (res != VK_SUCCESS)
      {
         instance_data.disp.DestroySurfaceKHR(instance, *pSurface, pAllocator);
      }
   }
   return res;
}

PFN_vkVoidFunction surface_properties::get_proc_addr(const char *name)
{
   if (strcmp(name, "vkGetPhysicalDeviceXcbPresentationSupportKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceXcbPresentationSupportKHR);
   }
   else if (strcmp(name, "vkCreateXcbSurfaceKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(CreateXcbSurfaceKHR);
   } else if (strcmp(name, "vkCreateXlibSurfaceKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(CreateXlibSurfaceKHR);
   } else if (strcmp(name, "vkGetPhysicalDeviceXlibPresentationSupportKHR") == 0)
   {
      return reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceXlibPresentationSupportKHR);
   }
   return nullptr;
}

bool surface_properties::is_surface_extension_enabled(const layer::instance_private_data &instance_data)
{
   return instance_data.is_instance_extension_enabled(VK_KHR_XCB_SURFACE_EXTENSION_NAME) ||
          instance_data.is_instance_extension_enabled(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
}

void surface_properties::get_surface_present_scaling_and_gravity(
   VkSurfacePresentScalingCapabilitiesEXT *scaling_capabilities)
{
   scaling_capabilities->supportedPresentScaling = VK_PRESENT_SCALING_ONE_TO_ONE_BIT_EXT;
   scaling_capabilities->supportedPresentGravityX = VK_PRESENT_GRAVITY_MIN_BIT_EXT;
   scaling_capabilities->supportedPresentGravityY = VK_PRESENT_GRAVITY_MIN_BIT_EXT;
}

bool surface_properties::is_compatible_present_modes(VkPresentModeKHR present_mode_a, VkPresentModeKHR present_mode_b)
{
   return m_compatible_present_modes.is_compatible_present_modes(present_mode_a, present_mode_b);
}

#if VULKAN_WSI_LAYER_EXPERIMENTAL
void surface_properties::get_present_timing_surface_caps(
   VkPresentTimingSurfaceCapabilitiesEXT *present_timing_surface_caps)
{
   present_timing_surface_caps->presentTimingSupported = VK_TRUE;
   present_timing_surface_caps->presentAtAbsoluteTimeSupported = VK_FALSE;
   present_timing_surface_caps->presentAtRelativeTimeSupported = VK_FALSE;
   present_timing_surface_caps->presentStageQueries = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
   present_timing_surface_caps->presentStageTargets = 0;
}
#endif

} // namespace xcb
} // namespace wsi
