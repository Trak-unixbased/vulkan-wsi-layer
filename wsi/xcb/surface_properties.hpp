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


#include "wsi/surface_properties.hpp"
#include "util/unordered_set.hpp"
#include "xcb_helpers.hpp"
#include "wsi/compatible_present_modes.hpp"

namespace wsi
{
namespace xcb
{

struct surface_format_properties_hasher
{
   size_t operator()(const VkFormat &format) const
   {
      return std::hash<uint64_t>()(static_cast<uint64_t>(format));
   }
};

using surface_format_properties_map = util::unordered_map<VkFormat, surface_format_properties, surface_format_properties_hasher>;

class surface;

class surface_properties : public wsi::surface_properties
{
public:
   surface_properties(surface *wsi_surface, const util::allocator &alloc);
   static surface_properties instance;

   static surface_properties &get_instance();

   VkResult get_surface_capabilities(VkPhysicalDevice physical_device,
                                     VkSurfaceCapabilitiesKHR *pSurfaceCapabilities) override;
   VkResult get_surface_capabilities(VkPhysicalDevice physical_device,
                                     const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                     VkSurfaceCapabilities2KHR *pSurfaceCapabilities) override;
   VkResult get_surface_formats(VkPhysicalDevice physical_device, uint32_t *surfaceFormatCount,
                                VkSurfaceFormatKHR *surfaceFormats,
                                VkSurfaceFormat2KHR *extended_surface_formats) override;
   VkResult get_surface_present_modes(VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                      uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes) override;

   VkResult get_required_device_extensions(util::extension_list &extension_list) override;

   VkResult get_required_instance_extensions(util::extension_list &extension_list) override;

   PFN_vkVoidFunction get_proc_addr(const char *name) override;

   bool is_surface_extension_enabled(const layer::instance_private_data &instance_data) override;

   bool is_compatible_present_modes(VkPresentModeKHR present_mode_a, VkPresentModeKHR present_mode_b) override;

#if VULKAN_WSI_LAYER_EXPERIMENTAL
   void get_present_timing_surface_caps(VkPresentTimingSurfaceCapabilitiesEXT *present_timing_surface_caps) override;
#endif
private:
   surface_properties();
   surface *specific_surface;
   /** Set of supported Vulkan formats by the @ref specific_surface. */
   surface_format_properties_map supported_formats;

   /* List of supported presentation modes */
   std::array<VkPresentModeKHR, 2> m_supported_modes;

   /* Stores compatible presentation modes */
   compatible_present_modes<2> m_compatible_present_modes;

   void populate_present_mode_compatibilities() override;

   void get_surface_present_scaling_and_gravity(VkSurfacePresentScalingCapabilitiesEXT *scaling_capabilities) override;

};

} // namespace xcb
} // namespace wsi
