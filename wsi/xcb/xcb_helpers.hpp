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

#pragma once

#include <stdint.h>

#include <X11/Xlib-xcb.h>
extern "C" {
#include <X11/xshmfence.h>
}
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shm.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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

struct connection_caps
{
   bool has_dri3;
   bool has_present;
};

void connection_get_caps(xcb_connection_t *conn, struct connection_caps *conn_caps);

bool visual_supported(xcb_visualtype_t *visual);

xcb_visualtype_t *
screen_get_visualtype(xcb_screen_t *screen,
                      xcb_visualid_t visual_id,
                      unsigned *depth);

xcb_visualtype_t *
connection_get_visualtype(xcb_connection_t *conn,
                          xcb_visualid_t visual_id);

xcb_screen_t *
get_screen_for_root(xcb_connection_t *conn, xcb_window_t root);

xcb_visualtype_t *
get_visualtype_for_window(xcb_connection_t *conn, xcb_window_t window,
                          unsigned *depth);

bool
visual_has_alpha(xcb_visualtype_t *visual, unsigned depth);

void
set_adaptive_sync_property(xcb_connection_t *conn,
                           xcb_drawable_t drawable,
                           uint32_t state);



