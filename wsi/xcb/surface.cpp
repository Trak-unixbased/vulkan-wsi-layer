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

#define VK_USE_PLATFORM_XCB_KHR 1
#define VK_USE_PLATFORM_XLIB_KHR 1


#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shm.h>
extern "C" {
#include <X11/xshmfence.h>
}
#include "surface.hpp"
#include "swapchain.hpp"
#include "surface_properties.hpp"

#include "util/log.hpp"
#include <cstring>

namespace wsi
{
namespace xcb
{

struct formats_vector
{
   util::vector<drm_format_pair> *formats{ nullptr };
   bool is_out_of_memory{ false };
};

struct surface::init_parameters
{
   const util::allocator &allocator;
   xcb_connection_t      *connection;
   xcb_window_t           window;
};

uint32_t drm_format_for_depth(uint32_t depth, uint32_t bpp)
{
   switch (bpp)
   {
   case 16:
      return DRM_FORMAT_RGB565;
   case 24:
      return DRM_FORMAT_XRGB8888;
   case 30:
      return DRM_FORMAT_XRGB2101010;
   case 32:
      return DRM_FORMAT_ARGB8888;
   default:
      return 0;
   }
}

static VkResult get_supported_formats_and_modifiers(const util::allocator         &allocator,
                                                    xcb_connection_t              *connection,
                                                    xcb_window_t                   window,
                                                    uint32_t                      &bit_depth,
                                                    uint32_t                      &bpp,
                                                    util::vector<drm_format_pair> &supported_formats)
{
   drm_format_pair drm_format = {};

   xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(connection,
                                                               xcb_get_geometry(connection, window),
                                                               NULL);

   if (geometry == NULL)
      return VK_ERROR_SURFACE_LOST_KHR;

   bit_depth = geometry->depth;
   bpp = 32;

   free(geometry);

   xcb_generic_error_t                       *error     = nullptr;
   xcb_dri3_get_supported_modifiers_cookie_t mod_cookie =
      xcb_dri3_get_supported_modifiers(connection, window, bit_depth, bpp);
   xcb_dri3_get_supported_modifiers_reply_t *mod_reply  =
      xcb_dri3_get_supported_modifiers_reply(connection, mod_cookie, &error);

   free(error);

   if (!mod_reply || (mod_reply->num_window_modifiers == 0 && mod_reply->num_screen_modifiers == 0))
   {
      if (mod_reply)
      {
         free(mod_reply);
      }

      // Xorg doesn't support modifiers by default, so we need to hardcode the drm_format and modifier here.
      drm_format.fourcc   = DRM_FORMAT_ARGB8888;
      drm_format.modifier = 0;
      supported_formats.try_push_back(drm_format);

      return VK_SUCCESS;
   }

   uint32_t n             = 0;
   uint32_t counts[2]     = {};
   uint64_t *modifiers[2] = {};
   uint32_t format        = 0;

   if (mod_reply->num_window_modifiers)
   {
      counts[n]    = mod_reply->num_window_modifiers;
      modifiers[n] = allocator.create<uint64_t>(counts[n]);

      if (!modifiers[n])
      {
         free(mod_reply);
         return VK_ERROR_UNKNOWN;
      }

      memcpy(modifiers[n], xcb_dri3_get_supported_modifiers_window_modifiers(mod_reply),
                  counts[n] * sizeof(uint64_t));
      n++;
   }

   if (mod_reply->num_screen_modifiers)
   {
      counts[n]    = mod_reply->num_screen_modifiers;
      modifiers[n] = allocator.create<uint64_t>(counts[n]);

      if (!modifiers[n])
      {
         if (n > 0)
         {
            allocator.destroy(counts[0], modifiers[0]);
         }
         free(mod_reply);
         return VK_ERROR_UNKNOWN;
      }

      memcpy(modifiers[n], xcb_dri3_get_supported_modifiers_screen_modifiers(mod_reply),
             counts[n] * sizeof(uint64_t));
      n++;
   }

   format = drm_format_for_depth(bit_depth, bpp);

   drm_format.fourcc = format;

   for (uint32_t i = 0; i < n; i++)
   {
      for (uint32_t j = 0; j < counts[i]; j++)
      {
         drm_format.modifier = modifiers[i][j];
         supported_formats.try_push_back(drm_format);
      }
      allocator.destroy(counts[i], modifiers[i]);
   }

   free(mod_reply);
   return VK_SUCCESS;
}

surface::surface(const init_parameters &params)
   : wsi::surface()
   , supported_formats(params.allocator)
   , properties(this, params.allocator)
   , present_pending(false)
   , connection(params.connection)
   , window(params.window)
   , bit_depth(0)
   , bpp(0)
{
}

bool surface::init(const util::allocator &allocator)
{
   VkResult vk_res = get_supported_formats_and_modifiers(allocator,
                                                         connection,
                                                         window,
                                                         bit_depth,
                                                         bpp, 
                                                         supported_formats);
   if (vk_res != VK_SUCCESS)
   {
      return false;
   }
   return true;
}

util::unique_ptr<surface> surface::make_surface(const util::allocator &allocator, xcb_connection_t *connection,
                                                xcb_window_t window)
{
   init_parameters params{ allocator, connection, window };
   auto wsi_surface = allocator.make_unique<surface>(params);
   if (wsi_surface != nullptr)
   {
      if (wsi_surface->init(allocator))
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

static void frame_done(void *data, uint32_t time)
{
   (void)time;

   bool *present_pending = reinterpret_cast<bool *>(data);
   assert(present_pending);

   *present_pending = false;
}

bool surface::set_frame_callback()
{
   return true;
}

bool surface::wait_next_frame_event()
{
   return true;
}

} // namespace xcb
} // namespace wsi
