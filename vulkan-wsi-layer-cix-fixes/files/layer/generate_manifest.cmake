# Copyright (c) 2026 vulkan-wsi-layer contributors.
#
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# Generates the layer manifest from layer/VkLayer_window_system_integration.json.
#
# Usage:
#   cmake -DINPUT=<template.json> -DOUTPUT=<manifest.json>
#         [-DREMOVE_EXTENSIONS=ext1|ext2|...] [-DLIBRARY_PATH=<path>]
#         -P generate_manifest.cmake
#
# Extensions are removed as whole JSON objects. The previous implementation used "sed /name/d", which only
# deleted the "name" line of multi-line entries (VK_EXT_present_timing, VK_KHR_present_wait) and left
# nameless objects in the manifest.

cmake_minimum_required(VERSION 3.19)

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
   message(FATAL_ERROR "generate_manifest.cmake: INPUT and OUTPUT must be defined")
endif()

file(READ "${INPUT}" manifest)

set(remove_list "")
if(DEFINED REMOVE_EXTENSIONS AND NOT REMOVE_EXTENSIONS STREQUAL "")
   string(REPLACE "|" ";" remove_list "${REMOVE_EXTENSIONS}")
endif()

foreach(section IN ITEMS instance_extensions device_extensions)
   string(JSON count ERROR_VARIABLE json_error LENGTH "${manifest}" layer ${section})
   if(json_error)
      continue()
   endif()

   # Walk backwards so that removing an element does not shift the indices still to be visited.
   math(EXPR index "${count} - 1")
   while(index GREATER_EQUAL 0)
      string(JSON ext_name GET "${manifest}" layer ${section} ${index} name)
      if(ext_name IN_LIST remove_list)
         string(JSON manifest REMOVE "${manifest}" layer ${section} ${index})
      endif()
      math(EXPR index "${index} - 1")
   endwhile()
endforeach()

if(DEFINED LIBRARY_PATH AND NOT LIBRARY_PATH STREQUAL "")
   string(JSON manifest SET "${manifest}" layer library_path "\"${LIBRARY_PATH}\"")
endif()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT}" "${manifest}\n")
