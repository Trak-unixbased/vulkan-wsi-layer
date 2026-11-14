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
 * Copyright (c) 2017-2019, 2021 Arm Limited.
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

/**
 * @file xcb_helpers.hpp
 *
 * @brief Small X11/XCB helpers used by the XCB WSI backend.
 */

#pragma once

#include <cstdint>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

namespace wsi
{
namespace xcb
{

/**
 * @brief Capabilities of an X server connection relevant to the WSI backend.
 */
struct connection_caps
{
   /** DRI3 extension available. */
   bool has_dri3{ false };
   /** DRI3 >= 1.2: PixmapFromBuffers and GetSupportedModifiers (multi-plane, format modifiers). */
   bool has_dri3_modifiers{ false };
   /** Present extension available. */
   bool has_present{ false };
   /** The server is Xwayland (detected through the XWAYLAND extension, Xwayland >= 23.1). */
   bool is_xwayland{ false };
};

/**
 * @brief Query the capabilities of an X server connection.
 *
 * @param      conn      The XCB connection.
 * @param[out] conn_caps The capabilities. All fields are false on failure.
 *
 * @return false if the connection is broken or the server did not answer, true otherwise.
 */
bool connection_get_caps(xcb_connection_t *conn, connection_caps *conn_caps);

bool visual_supported(const xcb_visualtype_t *visual);

xcb_visualtype_t *screen_get_visualtype(xcb_screen_t *screen, xcb_visualid_t visual_id, unsigned *depth);

xcb_visualtype_t *connection_get_visualtype(xcb_connection_t *conn, xcb_visualid_t visual_id);

xcb_screen_t *get_screen_for_root(xcb_connection_t *conn, xcb_window_t root);

xcb_visualtype_t *get_visualtype_for_window(xcb_connection_t *conn, xcb_window_t window, unsigned *depth);

/**
 * @brief Returns true if the visual has bits that are not used by the RGB channels (i.e. an alpha channel).
 */
bool visual_has_alpha(const xcb_visualtype_t *visual, unsigned depth);

/**
 * @brief Returns true if the window still exists on the server. Performs a round trip.
 */
bool window_exists(xcb_connection_t *conn, xcb_window_t window);

void set_adaptive_sync_property(xcb_connection_t *conn, xcb_drawable_t drawable, uint32_t state);

} // namespace xcb
} // namespace wsi
