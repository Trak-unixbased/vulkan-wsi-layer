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

#include "xcb_helpers.hpp"

#include <cstdlib>
#include <cstring>

namespace wsi
{
namespace xcb
{

bool connection_get_caps(xcb_connection_t *conn, connection_caps *conn_caps)
{
   *conn_caps = connection_caps{};

   if (conn == nullptr || xcb_connection_has_error(conn))
   {
      return false;
   }

   static const char dri3_name[] = "DRI3";
   static const char present_name[] = "Present";
   static const char xwayland_name[] = "XWAYLAND";

   /* Send all the requests first, then collect the replies: one round trip instead of three. */
   const xcb_query_extension_cookie_t dri3_cookie = xcb_query_extension(conn, strlen(dri3_name), dri3_name);
   const xcb_query_extension_cookie_t pres_cookie = xcb_query_extension(conn, strlen(present_name), present_name);
   const xcb_query_extension_cookie_t xwl_cookie = xcb_query_extension(conn, strlen(xwayland_name), xwayland_name);

   xcb_query_extension_reply_t *dri3_reply = xcb_query_extension_reply(conn, dri3_cookie, nullptr);
   xcb_query_extension_reply_t *pres_reply = xcb_query_extension_reply(conn, pres_cookie, nullptr);
   xcb_query_extension_reply_t *xwl_reply = xcb_query_extension_reply(conn, xwl_cookie, nullptr);

   /* Replies are NULL when the connection breaks: never dereference them blindly. */
   const bool answered = (dri3_reply != nullptr) && (pres_reply != nullptr);
   conn_caps->has_dri3 = (dri3_reply != nullptr) && dri3_reply->present;
   conn_caps->has_present = (pres_reply != nullptr) && pres_reply->present;
   conn_caps->is_xwayland = (xwl_reply != nullptr) && xwl_reply->present;

   free(dri3_reply);
   free(pres_reply);
   free(xwl_reply);

   if (conn_caps->has_dri3)
   {
      /* Advertise the highest version we know about; the server answers with the version it supports. */
      xcb_dri3_query_version_reply_t *ver =
         xcb_dri3_query_version_reply(conn, xcb_dri3_query_version(conn, 1, 2), nullptr);
      if (ver != nullptr)
      {
         conn_caps->has_dri3_modifiers =
            (ver->major_version > 1) || (ver->major_version == 1 && ver->minor_version >= 2);
         free(ver);
      }
   }

   if (conn_caps->has_present)
   {
      /* Present requires the client to announce its version before using the extension. */
      xcb_present_query_version_reply_t *ver =
         xcb_present_query_version_reply(conn, xcb_present_query_version(conn, 1, 2), nullptr);
      if (ver == nullptr)
      {
         conn_caps->has_present = false;
      }
      free(ver);
   }

   return answered;
}

bool visual_supported(const xcb_visualtype_t *visual)
{
   if (visual == nullptr)
   {
      return false;
   }

   return visual->_class == XCB_VISUAL_CLASS_TRUE_COLOR || visual->_class == XCB_VISUAL_CLASS_DIRECT_COLOR;
}

xcb_visualtype_t *screen_get_visualtype(xcb_screen_t *screen, xcb_visualid_t visual_id, unsigned *depth)
{
   xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);

   for (; depth_iter.rem; xcb_depth_next(&depth_iter))
   {
      xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);

      for (; visual_iter.rem; xcb_visualtype_next(&visual_iter))
      {
         if (visual_iter.data->visual_id == visual_id)
         {
            if (depth != nullptr)
            {
               *depth = depth_iter.data->depth;
            }
            return visual_iter.data;
         }
      }
   }

   return nullptr;
}

xcb_visualtype_t *connection_get_visualtype(xcb_connection_t *conn, xcb_visualid_t visual_id)
{
   xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(xcb_get_setup(conn));

   /* For this we have to iterate over all of the screens which is rather
    * annoying.  Fortunately, there is probably only 1.
    */
   for (; screen_iter.rem; xcb_screen_next(&screen_iter))
   {
      xcb_visualtype_t *visual = screen_get_visualtype(screen_iter.data, visual_id, nullptr);
      if (visual != nullptr)
      {
         return visual;
      }
   }

   return nullptr;
}

xcb_screen_t *get_screen_for_root(xcb_connection_t *conn, xcb_window_t root)
{
   xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(xcb_get_setup(conn));

   for (; screen_iter.rem; xcb_screen_next(&screen_iter))
   {
      if (screen_iter.data->root == root)
      {
         return screen_iter.data;
      }
   }

   return nullptr;
}

xcb_visualtype_t *get_visualtype_for_window(xcb_connection_t *conn, xcb_window_t window, unsigned *depth)
{
   const xcb_query_tree_cookie_t tree_cookie = xcb_query_tree(conn, window);
   const xcb_get_window_attributes_cookie_t attrib_cookie = xcb_get_window_attributes(conn, window);

   xcb_query_tree_reply_t *tree = xcb_query_tree_reply(conn, tree_cookie, nullptr);
   xcb_get_window_attributes_reply_t *attrib = xcb_get_window_attributes_reply(conn, attrib_cookie, nullptr);
   if (attrib == nullptr || tree == nullptr)
   {
      free(attrib);
      free(tree);
      return nullptr;
   }

   const xcb_window_t root = tree->root;
   const xcb_visualid_t visual_id = attrib->visual;
   free(attrib);
   free(tree);

   xcb_screen_t *screen = get_screen_for_root(conn, root);
   if (screen == nullptr)
   {
      return nullptr;
   }

   return screen_get_visualtype(screen, visual_id, depth);
}

bool visual_has_alpha(const xcb_visualtype_t *visual, unsigned depth)
{
   if (visual == nullptr || depth == 0 || depth > 32)
   {
      return false;
   }

   const uint32_t rgb_mask = visual->red_mask | visual->green_mask | visual->blue_mask;
   const uint32_t all_mask = 0xffffffffu >> (32 - depth);

   /* Do we have bits left over after RGB? */
   return (all_mask & ~rgb_mask) != 0;
}

bool window_exists(xcb_connection_t *conn, xcb_window_t window)
{
   if (xcb_connection_has_error(conn))
   {
      return false;
   }

   xcb_generic_error_t *error = nullptr;
   xcb_get_window_attributes_reply_t *reply =
      xcb_get_window_attributes_reply(conn, xcb_get_window_attributes(conn, window), &error);
   const bool exists = (reply != nullptr);
   free(reply);
   free(error);
   return exists;
}

void set_adaptive_sync_property(xcb_connection_t *conn, xcb_drawable_t drawable, uint32_t state)
{
   static char const name[] = "_VARIABLE_REFRESH";

   xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, strlen(name), name), nullptr);
   if (reply == nullptr)
   {
      return;
   }

   xcb_void_cookie_t check;
   if (state)
   {
      check = xcb_change_property_checked(conn, XCB_PROP_MODE_REPLACE, drawable, reply->atom, XCB_ATOM_CARDINAL, 32, 1,
                                          &state);
   }
   else
   {
      check = xcb_delete_property_checked(conn, drawable, reply->atom);
   }

   xcb_discard_reply(conn, check.sequence);
   free(reply);
}

} // namespace xcb
} // namespace wsi
