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
 * @brief Definitions for a XCB WSI Surface
 */

#pragma once

#include "wsi/surface.hpp"
#include "surface_properties.hpp"

#include "util/macros.hpp"

namespace wsi
{
namespace xcb
{

struct drm_format_pair
{
   uint32_t fourcc;
   uint64_t modifier;
};


class surface : public wsi::surface
{
public:
   surface() = delete;
   struct init_parameters;

   /** Constructor to allow for custom allocation, but require privately defined arguments. */
   surface(const init_parameters&);

   static util::unique_ptr<surface> make_surface(const util::allocator &allocator, xcb_connection_t* connection, xcb_window_t window);

   /** Destructor */
   ~surface() override;

   wsi::surface_properties &get_properties() override;
   util::unique_ptr<swapchain_base> allocate_swapchain(layer::device_private_data &dev_data,
                                                       const VkAllocationCallbacks *allocator) override;

   /**
    * @brief Get the XCB connection associated with the surface.
    *
    * @return The XCB connection.
    */
   xcb_connection_t* get_xcb_connection() const
   {
      return connection;
   }

   /**
    * @brief Get the XCB window associated with the surface.
    *
    * @return The XCB window.
    */
   xcb_window_t get_xcb_window() const
   {
      return window;
   }

   /**
    * @brief Get the bit depth of the surface.
    *
    * @return The bit depth of the surface.
    */
   uint32_t get_bit_depth() const
   {
      return bit_depth;
   }

   /**
    * @brief Get the bits per pixel (bpp) of the surface.
    *
    * @return The number of bits per pixel.
    */
   uint32_t get_bpp() const
   {
      return bpp;
   }

   /**
    * @brief Returns a reference to a list of DRM formats supported by the XCB surface.
    *
    * The reference is valid throughout the lifetime of this surface.
    */
   const util::vector<drm_format_pair> &get_formats() const
   {
      return supported_formats;
   }

   /**
    * @brief Set the next frame callback.
    *
    * Make a frame request on the compositor which will be applied in the next
    * wl_surface::commit. It overwrites previous requested frame events.
    *
    * @return true for success, false otherwise.
    */
   bool set_frame_callback();

   /**
    * @brief Wait for the compositor's last requested frame event.
    *
    * @return true for success, false otherwise.
    */
   bool wait_next_frame_event();

private:
   /**
    * @brief Initialize the WSI surface
    *
    * @return true on success, false otherwise.
    */
   bool init(const util::allocator &allocator);

   friend void surface_registry_handler(void *data, uint32_t name,
                                        const char *interface, uint32_t version) VWL_API_POST;

   /** A list of DRM formats supported by the compositor on this surface */
   util::vector<drm_format_pair> supported_formats;
   /** Surface properties specific to the xcb surface. */
   surface_properties properties;

   /**
    * @brief true when waiting for the server hint to present a buffer
    *
    * true if a buffer has been presented and we've not had a wl_surface::frame
    * callback to indicate the server is ready for the next buffer.
    */
   bool present_pending;

   xcb_connection_t *connection;
   xcb_window_t      window;
   uint32_t          bit_depth;
   uint32_t          bpp;
};

} // namespace xcb
} // namespace wsi
