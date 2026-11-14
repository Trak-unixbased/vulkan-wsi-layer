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
 * Copyright (c) 2021 Arm Limited.
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

/** @file
 * @brief Implementation of a Xcb WSI Surface
 */

#include <cstdlib>
#include <cstring>

#include <xcb/xcb.h>
#include <xcb/dri3.h>

#include <drm_fourcc.h>

#include "surface.hpp"
#include "swapchain.hpp"
#include "surface_properties.hpp"

#include "util/log.hpp"

namespace wsi
{
namespace xcb
{

struct surface::init_parameters
{
   const util::allocator &allocator;
   xcb_connection_t *connection;
   xcb_window_t window;
};

/**
 * @brief DRM fourcc used for pixmaps of a window of the given depth.
 *
 * X only cares about depth/bpp; the fourcc is what the Vulkan side allocates and imports. Depth 24 and 32 windows
 * both use 32 bpp B8G8R8A8-ordered pixmaps: the X server simply ignores the top byte at depth 24.
 * The previous implementation switched on the bpp (always 32) and therefore ignored the depth entirely.
 */
static uint32_t drm_format_for_depth(uint32_t depth)
{
   switch (depth)
   {
   case 16:
      return DRM_FORMAT_RGB565;
   case 24:
   case 32:
      return DRM_FORMAT_ARGB8888;
   case 30:
      return DRM_FORMAT_ARGB2101010;
   default:
      return 0;
   }
}

static bool push_format(util::vector<drm_format_pair> &formats, uint32_t fourcc, uint64_t modifier)
{
   /* DRM_FORMAT_MOD_INVALID is not a real layout and cannot be imported by the Mali driver. */
   if (modifier == DRM_FORMAT_MOD_INVALID)
   {
      return true;
   }
   for (const auto &format : formats)
   {
      if (format.fourcc == fourcc && format.modifier == modifier)
      {
         return true;
      }
   }
   return formats.try_push_back(drm_format_pair{ fourcc, modifier });
}

static VkResult get_supported_formats_and_modifiers(xcb_connection_t *connection, xcb_window_t window,
                                                    const connection_caps &caps, uint32_t &bit_depth, uint32_t &bpp,
                                                    util::vector<drm_format_pair> &supported_formats)
{
   xcb_get_geometry_reply_t *geometry =
      xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr);
   if (geometry == nullptr)
   {
      return VK_ERROR_SURFACE_LOST_KHR;
   }

   bit_depth = geometry->depth;
   bpp = (bit_depth == 16) ? 16 : 32;
   free(geometry);

   const uint32_t fourcc = drm_format_for_depth(bit_depth);
   if (fourcc == 0)
   {
      WSI_LOG_WARNING("Unsupported X window depth %u.", bit_depth);
      return VK_SUCCESS;
   }

   /* GetSupportedModifiers is a DRI3 1.2 request: only use it when the server supports it. */
   if (caps.has_dri3_modifiers)
   {
      xcb_generic_error_t *error = nullptr;
      xcb_dri3_get_supported_modifiers_reply_t *mod_reply = xcb_dri3_get_supported_modifiers_reply(
         connection, xcb_dri3_get_supported_modifiers(connection, window, bit_depth, bpp), &error);
      free(error);

      if (mod_reply != nullptr)
      {
         /* Window modifiers first: they allow direct scanout of the window, screen modifiers only composition. */
         const uint64_t *window_mods = xcb_dri3_get_supported_modifiers_window_modifiers(mod_reply);
         for (uint32_t i = 0; i < mod_reply->num_window_modifiers; i++)
         {
            if (!push_format(supported_formats, fourcc, window_mods[i]))
            {
               free(mod_reply);
               return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
         }

         const uint64_t *screen_mods = xcb_dri3_get_supported_modifiers_screen_modifiers(mod_reply);
         for (uint32_t i = 0; i < mod_reply->num_screen_modifiers; i++)
         {
            if (!push_format(supported_formats, fourcc, screen_mods[i]))
            {
               free(mod_reply);
               return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
         }
         free(mod_reply);
      }
   }

   /* Xorg without modifier support, or DRI3 < 1.2: linear buffers are always importable by the X server. */
   if (supported_formats.empty())
   {
      if (!push_format(supported_formats, fourcc, DRM_FORMAT_MOD_LINEAR))
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   return VK_SUCCESS;
}

surface::surface(const init_parameters &params)
   : wsi::surface()
   , supported_formats(params.allocator)
   , properties(this, params.allocator)
   , connection(params.connection)
   , window(params.window)
   , bit_depth(0)
   , bpp(0)
   , caps()
{
}

bool surface::init()
{
   if (!connection_get_caps(connection, &caps))
   {
      WSI_LOG_ERROR("Failed to query the X server capabilities.");
      return false;
   }

   if (!caps.has_dri3 || !caps.has_present)
   {
      /* The surface can still be created; vkGetPhysicalDeviceSurfaceSupportKHR will report it as unsupported. */
      WSI_LOG_WARNING("X server lacks DRI3 or Present: presentation will not be supported on this surface.");
   }

   return get_supported_formats_and_modifiers(connection, window, caps, bit_depth, bpp, supported_formats) ==
          VK_SUCCESS;
}

util::unique_ptr<surface> surface::make_surface(const util::allocator &allocator, xcb_connection_t *connection,
                                                xcb_window_t window)
{
   init_parameters params{ allocator, connection, window };
   auto wsi_surface = allocator.make_unique<surface>(params);
   if (wsi_surface != nullptr)
   {
      if (wsi_surface->init())
      {
         return wsi_surface;
      }
   }
   return nullptr;
}

surface::~surface()
{
}

wsi::surface_properties &surface::get_properties()
{
   return properties;
}

util::unique_ptr<swapchain_base> surface::allocate_swapchain(layer::device_private_data &dev_data,
                                                             const VkAllocationCallbacks *allocator)
{
   util::allocator alloc{ dev_data.get_allocator(), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, allocator };
   return util::unique_ptr<swapchain_base>(alloc.make_unique<swapchain>(dev_data, allocator, *this));
}

} // namespace xcb
} // namespace wsi
