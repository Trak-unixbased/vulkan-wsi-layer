# Copyright (c) 2019-2025 Arm Limited.
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

# CMake 4.x (Ubuntu 26.04 ships 4.2) removed compatibility with projects declaring a minimum below 3.5.
# The <min>...<max> form keeps the project buildable with both CMake 3.x and 4.x.
cmake_minimum_required(VERSION 3.19...4.2)
project(VkLayer_window_system_integration C CXX)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(VULKAN_PKG_CONFIG vulkan)

option(ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
if(ENABLE_CLANG_TIDY)
   find_program(CLANG_TIDY NAMES clang-tidy clang-tidy-8)
   if(CLANG_TIDY)
      message(STATUS "Using clang-tidy: ${CLANG_TIDY}")
      set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY} -checks=bugprone-*,modernize-*)
   endif()
endif()

option(WSI_LAYER_WERROR "Treat compiler warnings as errors" OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread -fPIC")
if(WSI_LAYER_WERROR)
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
endif()
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(THIRDPARTY_INCLUDE ${THIRDPARTY_DIR}/include)

if(NOT DEFINED VULKAN_CXX_INCLUDE)
   set(VULKAN_CXX_INCLUDE ${VULKAN_PKG_CONFIG_INCLUDEDIR})
endif()

if(DEFINED VULKAN_CXX_INCLUDE)
   message(STATUS "Using Vulkan include directories: ${VULKAN_CXX_INCLUDE}")
   separate_arguments(VULKAN_CXX_INCLUDE)
else()
   message(FATAL_ERROR "Either vulkan.pc must be available or VULKAN_CXX_INCLUDE must be defined")
endif()

# Locate vulkan_core.h to check the header version against the experimental code.
find_file(VULKAN_CORE_HEADER vulkan/vulkan_core.h PATHS ${VULKAN_CXX_INCLUDE} NO_DEFAULT_PATH)
if(NOT VULKAN_CORE_HEADER)
   find_file(VULKAN_CORE_HEADER vulkan/vulkan_core.h)
endif()

# Build Configuration options

# Backend support
option(BUILD_WSI_HEADLESS "Build with support for VK_EXT_headless_surface" ON)
option(BUILD_WSI_WAYLAND "Build with support for VK_KHR_wayland_surface" OFF)
option(BUILD_WSI_DISPLAY "Build with support for VK_KHR_display" OFF)
option(BUILD_WSI_XCB "Build with support for VK_KHR_xcb_surface" OFF)

set(SELECT_EXTERNAL_ALLOCATOR "none" CACHE STRING "Select an external system allocator (none, ion, dma_buf_heaps)")
set(EXTERNAL_WSIALLOC_LIBRARY "" CACHE STRING "External implementation of the wsialloc interface to use")
set(WSIALLOC_MEMORY_HEAP_NAME "linux,cma" CACHE STRING "Preferred heap name used by the dma_buf_heaps allocator")
set(WSIALLOC_FALLBACK_HEAP_NAME "system" CACHE STRING
    "Heap used by the dma_buf_heaps allocator when the preferred heap is missing or exhausted (empty to disable)")
option(WSI_LAYER_INSTALL_IMPLICIT "Install the layer manifest as an implicit layer" ON)

# Optional features
option(BUILD_WSI_DISPLAY_SUPPORT_FORMAT_MODIFIERS "Build with support for format modifiers in VK_KHR_display" ON)
option(VULKAN_WSI_LAYER_EXPERIMENTAL "Enable the Vulkan WSI Experimental features" OFF)
option(ENABLE_WAYLAND_FIFO_PRESENTATION_THREAD "Enable the non-conformant FIFO presentation thread implementation in the Wayland backend" OFF)
option(WAYLAND_SUPPORT_EXPLICIT_SYNC "Enable Wayland linux-explicit-synchronization protocol support" OFF)

# Enables the layer to pass frame boundary events if the ICD or layers below have support for it by
# making use of the VK_EXT_frame_boundary extension. If the application itself makes use of the
# VK_EXT_frame_boundary then the layer will not pass its own frame boundary events.
option(ENABLE_INSTRUMENTATION "Pass frame boundary events by using VK_EXT_frame_boundary" OFF)

# The experimental code declares its own provisional VK_EXT_present_timing API. Since Vulkan 1.4.33x the extension is
# part of the official headers with a different API (spec version 3), so both cannot coexist (Ubuntu 26.04 ships
# 1.4.341). Fail early with a clear message instead of a wall of redefinition errors.
if(VULKAN_WSI_LAYER_EXPERIMENTAL AND VULKAN_CORE_HEADER)
   file(STRINGS "${VULKAN_CORE_HEADER}" VULKAN_HAS_PRESENT_TIMING REGEX "^#define VK_EXT_present_timing 1")
   if(VULKAN_HAS_PRESENT_TIMING)
      message(FATAL_ERROR "VULKAN_WSI_LAYER_EXPERIMENTAL requires Vulkan headers without the official "
                          "VK_EXT_present_timing extension (${VULKAN_CORE_HEADER} defines it). "
                          "Build with -DVULKAN_WSI_LAYER_EXPERIMENTAL=OFF.")
   endif()
endif()

if(BUILD_WSI_WAYLAND OR BUILD_WSI_DISPLAY)
   set(BUILD_DRM_UTILS true)
   if(SELECT_EXTERNAL_ALLOCATOR STREQUAL "none")
      message(FATAL_ERROR "WSI only supported with an external allocator.")
   endif()
endif()

if(BUILD_WSI_XCB)
   set(BUILD_DRM_UTILS true)
   if(SELECT_EXTERNAL_ALLOCATOR STREQUAL "none")
      message(FATAL_ERROR "XCB only supported with an external allocator.")
   endif()
endif()

if(BUILD_DRM_UTILS)
   add_library(drm_utils STATIC util/drm/drm_utils.cpp)

   pkg_check_modules(LIBDRM REQUIRED libdrm)
   message(STATUS "Using libdrm include directories: ${LIBDRM_INCLUDE_DIRS}")
   message(STATUS "Using libdrm cflags: ${LIBDRM_CFLAGS}")

   target_sources(drm_utils PRIVATE util/drm/format_table.c)
   target_include_directories(drm_utils PRIVATE ${VULKAN_CXX_INCLUDE})
   target_include_directories(drm_utils PUBLIC ${LIBDRM_INCLUDE_DIRS})
   target_include_directories(drm_utils PUBLIC util/wsialloc)
   target_compile_options(drm_utils PUBLIC ${LIBDRM_CFLAGS})
endif()

# External WSI Allocator
if(NOT SELECT_EXTERNAL_ALLOCATOR STREQUAL "none" AND EXTERNAL_WSIALLOC_LIBRARY STREQUAL "")
   add_library(wsialloc STATIC)
   set_target_properties(wsialloc PROPERTIES C_STANDARD 99)

   if(SELECT_EXTERNAL_ALLOCATOR STREQUAL "ion")
      target_sources(wsialloc PRIVATE util/wsialloc/wsialloc_ion.c util/wsialloc/wsialloc_helpers.c)
      target_link_libraries(wsialloc drm_utils)
      if(DEFINED KERNEL_HEADER_DIR)
         target_include_directories(wsialloc PRIVATE "${KERNEL_HEADER_DIR}")
      else()
         message(FATAL_ERROR "KERNEL_HEADER_DIR must be defined as the directory that includes the kernel headers.")
      endif()
   elseif(SELECT_EXTERNAL_ALLOCATOR STREQUAL "dma_buf_heaps")
      target_sources(wsialloc PRIVATE util/wsialloc/wsialloc_dma_buf_heaps.c util/wsialloc/wsialloc_helpers.c)
      target_link_libraries(wsialloc drm_utils)
      # linux/dma-heap.h is part of the kernel UAPI (linux-libc-dev), so KERNEL_HEADER_DIR is optional here.
      if(DEFINED KERNEL_HEADER_DIR)
         target_include_directories(wsialloc PRIVATE "${KERNEL_HEADER_DIR}")
      endif()
      # Heap names are passed as string literals, which avoids the former "-Ulinux" hack that was needed
      # to stop the preprocessor from expanding the "linux" token of "linux,cma".
      target_compile_definitions(wsialloc PRIVATE
         "WSIALLOC_MEMORY_HEAP_NAME=\"${WSIALLOC_MEMORY_HEAP_NAME}\""
         "WSIALLOC_FALLBACK_HEAP_NAME=\"${WSIALLOC_FALLBACK_HEAP_NAME}\"")
   else()
      message(FATAL_ERROR "Invalid external allocator selected: ${SELECT_EXTERNAL_ALLOCATOR}")
   endif()

   target_include_directories(wsialloc PRIVATE ${VULKAN_CXX_INCLUDE})
   target_include_directories(wsialloc PRIVATE util/drm)
   target_include_directories(wsialloc PRIVATE util/)
endif()

# Wayland WSI
if(BUILD_WSI_WAYLAND)
   add_library(wayland_wsi STATIC
      wsi/wayland/surface_properties.cpp
      wsi/wayland/surface.cpp
      wsi/wayland/wl_helpers.cpp
      wsi/wayland/swapchain.cpp)

   if(VULKAN_WSI_LAYER_EXPERIMENTAL)
      target_sources(wayland_wsi PRIVATE wsi/wayland/present_id_wayland.cpp)
      target_sources(wayland_wsi PRIVATE wsi/wayland/present_timing_handler.cpp)
      target_sources(wayland_wsi PRIVATE wsi/wayland/wp_presentation_feedback.cpp)
   endif()

   pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
   message(STATUS "Using Wayland client include directories: ${WAYLAND_CLIENT_INCLUDE_DIRS}")
   message(STATUS "Using Wayland client cflags: ${WAYLAND_CLIENT_CFLAGS}")
   message(STATUS "Using Wayland client ldflags: ${WAYLAND_CLIENT_LDFLAGS}")

   find_program(WAYLAND_SCANNER_EXEC wayland-scanner REQUIRED)
   message(STATUS "Using wayland-scanner : ${WAYLAND_SCANNER_EXEC}")

   pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
   pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
   message(STATUS "Using wayland protocols dir : ${WAYLAND_PROTOCOLS_DIR}")

   add_custom_target(wayland_generated_files
      COMMAND ${WAYLAND_SCANNER_EXEC} client-header
      ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
      ${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.h
      COMMAND ${WAYLAND_SCANNER_EXEC} public-code
      ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
      ${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-protocol.c
      COMMAND ${WAYLAND_SCANNER_EXEC} client-header
      ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml
      ${CMAKE_CURRENT_BINARY_DIR}/linux-explicit-synchronization-unstable-v1-protocol.h
      COMMAND ${WAYLAND_SCANNER_EXEC} public-code
      ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml
      ${CMAKE_CURRENT_BINARY_DIR}/linux-explicit-synchronization-unstable-v1-protocol.c
      COMMAND ${WAYLAND_SCANNER_EXEC} client-header
      ${WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml
      ${CMAKE_CURRENT_BINARY_DIR}/presentation-time-client-protocol.h
      COMMAND ${WAYLAND_SCANNER_EXEC} public-code
      ${WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml
      ${CMAKE_CURRENT_BINARY_DIR}/presentation-time-client-protocol.c
      BYPRODUCTS linux-dmabuf-unstable-v1-protocol.c linux-dmabuf-unstable-v1-client-protocol.h
                 linux-explicit-synchronization-unstable-v1-protocol.c linux-explicit-synchronization-unstable-v1-protocol.h
                 presentation-time-client-protocol.c presentation-time-client-protocol.h)

   target_sources(wayland_wsi PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-protocol.c
      ${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.h
      ${CMAKE_CURRENT_BINARY_DIR}/linux-explicit-synchronization-unstable-v1-protocol.c
      ${CMAKE_CURRENT_BINARY_DIR}/linux-explicit-synchronization-unstable-v1-protocol.h
      ${CMAKE_CURRENT_BINARY_DIR}/presentation-time-client-protocol.c
      ${CMAKE_CURRENT_BINARY_DIR}/presentation-time-client-protocol.h)
   add_dependencies(wayland_wsi wayland_generated_files)

   target_include_directories(wayland_wsi PRIVATE
      ${PROJECT_SOURCE_DIR}
      ${VULKAN_CXX_INCLUDE}
      ${WAYLAND_CLIENT_INCLUDE_DIRS}
      ${CMAKE_CURRENT_BINARY_DIR}
      ${THIRDPARTY_INCLUDE})

   target_compile_options(wayland_wsi PRIVATE ${WAYLAND_CLIENT_CFLAGS})
   target_compile_options(wayland_wsi INTERFACE "-DBUILD_WSI_WAYLAND=1")
   if(NOT EXTERNAL_WSIALLOC_LIBRARY STREQUAL "")
      target_link_libraries(wayland_wsi ${EXTERNAL_WSIALLOC_LIBRARY})
   else()
      target_link_libraries(wayland_wsi wsialloc)
   endif()
   target_link_libraries(wayland_wsi drm_utils ${WAYLAND_CLIENT_LDFLAGS})
   list(APPEND LINK_WSI_LIBS wayland_wsi)
   if(ENABLE_WAYLAND_FIFO_PRESENTATION_THREAD)
      target_compile_definitions(wayland_wsi PRIVATE "-DWAYLAND_FIFO_PRESENTATION_THREAD_ENABLED=1")
   else()
      target_compile_definitions(wayland_wsi PRIVATE "-DWAYLAND_FIFO_PRESENTATION_THREAD_ENABLED=0")
   endif()
   if(WAYLAND_SUPPORT_EXPLICIT_SYNC)
      target_compile_definitions(wayland_wsi PRIVATE "-DSUPPORT_EXPLICIT_SYNC=1")
   else()
      target_compile_definitions(wayland_wsi PRIVATE "-DSUPPORT_EXPLICIT_SYNC=0")
   endif()
else()
   list(APPEND WSI_MANIFEST_REMOVED_EXTENSIONS VK_KHR_wayland_surface)
endif()

# Headless
if(BUILD_WSI_HEADLESS)
   add_library(wsi_headless STATIC
      wsi/headless/surface_properties.cpp
      wsi/headless/surface.cpp
      wsi/headless/swapchain.cpp)

   if(VULKAN_WSI_LAYER_EXPERIMENTAL)
      target_sources(wsi_headless PRIVATE wsi/headless/present_timing_handler.cpp)
   endif()

   target_include_directories(wsi_headless PRIVATE
      ${PROJECT_SOURCE_DIR}
      ${VULKAN_CXX_INCLUDE}
      ${THIRDPARTY_INCLUDE}
      ${CMAKE_CURRENT_BINARY_DIR})

   target_compile_options(wsi_headless INTERFACE "-DBUILD_WSI_HEADLESS=1")
   list(APPEND LINK_WSI_LIBS wsi_headless)
else()
   list(APPEND WSI_MANIFEST_REMOVED_EXTENSIONS VK_EXT_headless_surface VK_KHR_shared_presentable_image)
endif()

# Display
if (BUILD_WSI_DISPLAY)
   add_library(wsi_display STATIC
      wsi/display/drm_display.cpp
      wsi/display/surface_properties.cpp
      wsi/display/swapchain.cpp
      wsi/display/surface.cpp)

   pkg_check_modules(LIBDRM REQUIRED libdrm)
   message(STATUS "Using libdrm include directories: ${LIBDRM_INCLUDE_DIRS}")
   message(STATUS "Using libdrm ldflags: ${LIBDRM_LDFLAGS}")

   target_include_directories(wsi_display PRIVATE
      ${PROJECT_SOURCE_DIR}
      ${VULKAN_CXX_INCLUDE}
      ${CMAKE_CURRENT_BINARY_DIR})

   target_include_directories(wsi_display PUBLIC
      ${LIBDRM_INCLUDE_DIRS})

   target_compile_options(wsi_display INTERFACE "-DBUILD_WSI_DISPLAY=1")
   target_link_libraries(wsi_display ${LIBDRM_LDFLAGS} drm)
   target_link_libraries(wsi_display drm_utils)
   if(NOT EXTERNAL_WSIALLOC_LIBRARY STREQUAL "")
      target_link_libraries(wsi_display ${EXTERNAL_WSIALLOC_LIBRARY})
   else()
      target_link_libraries(wsi_display wsialloc)
   endif()
   list(APPEND LINK_WSI_LIBS wsi_display)
else()
   list(APPEND WSI_MANIFEST_REMOVED_EXTENSIONS VK_KHR_display)
endif()

# Xcb
if(BUILD_WSI_XCB)
   add_library(xcb_wsi STATIC
      wsi/xcb/surface_properties.cpp
      wsi/xcb/surface.cpp
      wsi/xcb/swapchain.cpp
      wsi/xcb/xcb_helpers.cpp)


   pkg_check_modules(XCB REQUIRED xcb)
   message(STATUS "Using xcb include directories: ${XCB_INCLUDE_DIRS}")
   message(STATUS "Using xcb cflags: ${XCB_CFLAGS}")
   message(STATUS "Using xcb ldflags: ${XCB_LDFLAGS}")

   pkg_check_modules(XCB_DRI3 REQUIRED xcb-dri3)
   message(STATUS "Using xcb-dri3 include directories: ${XCB_DRI3_INCLUDE_DIRS}")
   message(STATUS "Using xcb-dri3 cflags: ${XCB_DRI3_CFLAGS}")
   message(STATUS "Using xcb-dri3 ldflags: ${XCB_DRI3_LDFLAGS}")

   pkg_check_modules(XCB_PRESENT REQUIRED xcb-present)
   message(STATUS "Using xcb-present include directories: ${XCB_PRESENT_INCLUDE_DIRS}")
   message(STATUS "Using xcb-present cflags: ${XCB_PRESENT_CFLAGS}")
   message(STATUS "Using xcb-present ldflags: ${XCB_PRESENT_LDFLAGS}")

   pkg_check_modules(XSHMFENCE REQUIRED xshmfence)
   message(STATUS "Using xshmfence include directories: ${XSHMFENCE_INCLUDE_DIRS}")
   message(STATUS "Using xshmfence cflags: ${XSHMFENCE_CFLAGS}")
   message(STATUS "Using xshmfence ldflags: ${XSHMFENCE_LDFLAGS}")

   pkg_check_modules(XCB_SYNC REQUIRED xcb-sync)
   message(STATUS "Using xcb-sync include directories: ${XCB_SYNC_INCLUDE_DIRS}")
   message(STATUS "Using xcb-sync cflags: ${XCB_SYNC_CFLAGS}")
   message(STATUS "Using xcb-sync ldflags: ${XCB_SYNC_LDFLAGS}")

   pkg_check_modules(X11_XCB REQUIRED x11-xcb)
   message(STATUS "Using x11-xcb include directories: ${X11_XCB_INCLUDE_DIRS}")
   message(STATUS "Using x11-xcb cflags: ${X11_XCB_CFLAGS}")
   message(STATUS "Using x11-xcb ldflags: ${X11_XCB_LDFLAGS}")

   target_include_directories(xcb_wsi PRIVATE
      ${PROJECT_SOURCE_DIR}
      ${VULKAN_CXX_INCLUDE}
      ${THIRDPARTY_INCLUDE}
      ${LIBDRM_INCLUDE_DIRS}
      ${CMAKE_CURRENT_BINARY_DIR})

   target_compile_options(xcb_wsi PRIVATE ${XCB_CFLAGS} ${XCB_DRI3_CFLAGS} ${XCB_PRESENT_CFLAGS} ${XSHMFENCE_CFLAGS} ${XCB_SYNC_CFLAGS} ${X11_XCB_CFLAGS})
   target_compile_options(xcb_wsi INTERFACE "-DBUILD_WSI_XCB=1")
   if(NOT EXTERNAL_WSIALLOC_LIBRARY STREQUAL "")
      target_link_libraries(xcb_wsi ${EXTERNAL_WSIALLOC_LIBRARY})
   else()
      target_link_libraries(xcb_wsi wsialloc)
   endif()
   target_link_libraries(xcb_wsi drm_utils ${XCB_LDFLAGS} ${XCB_DRI3_LDFLAGS} ${XCB_PRESENT_LDFLAGS} ${XSHMFENCE_LDFLAGS} ${XCB_SYNC_LDFLAGS} ${X11_XCB_LDFLAGS})

   list(APPEND LINK_WSI_LIBS xcb_wsi)
else()
   # The manifest advertises both VK_KHR_xcb_surface and VK_KHR_xlib_surface: strip both when XCB is disabled.
   list(APPEND WSI_MANIFEST_REMOVED_EXTENSIONS VK_KHR_xcb_surface VK_KHR_xlib_surface)
endif()

# Layer
add_library(${PROJECT_NAME} SHARED
   layer/calibrated_timestamps_api.cpp
   layer/layer.cpp
   layer/private_data.cpp
   layer/surface_api.cpp
   layer/swapchain_api.cpp
   layer/swapchain_maintenance_api.cpp
   util/timed_semaphore.cpp
   util/custom_allocator.cpp
   util/extension_list.cpp
   util/log.cpp
   util/format_modifiers.cpp
   wsi/external_memory.cpp
   wsi/extensions/image_compression_control.cpp
   wsi/extensions/present_id.cpp
   wsi/extensions/frame_boundary.cpp
   wsi/extensions/wsi_extension.cpp
   wsi/extensions/swapchain_maintenance.cpp
   wsi/surface_properties.cpp
   wsi/swapchain_base.cpp
   wsi/synchronization.cpp
   wsi/wsi_factory.cpp)
if (VULKAN_WSI_LAYER_EXPERIMENTAL)
   target_sources(${PROJECT_NAME} PUBLIC ${PROJECT_SOURCE_DIR}/layer/present_timing_api.cpp)
   target_sources(${PROJECT_NAME} PUBLIC ${PROJECT_SOURCE_DIR}/wsi/extensions/present_timing.cpp)
   add_definitions("-DVULKAN_WSI_LAYER_EXPERIMENTAL=1")
else()
   list(APPEND WSI_MANIFEST_REMOVED_EXTENSIONS VK_EXT_present_timing VK_KHR_present_wait)
   add_definitions("-DVULKAN_WSI_LAYER_EXPERIMENTAL=0")
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE ${WSI_DEFINES})
target_include_directories(${PROJECT_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${VULKAN_CXX_INCLUDE})

if (BUILD_WSI_DISPLAY_SUPPORT_FORMAT_MODIFIERS)
   add_definitions("-DWSI_DISPLAY_SUPPORT_FORMAT_MODIFIERS=1")
else()
   add_definitions("-DWSI_DISPLAY_SUPPORT_FORMAT_MODIFIERS=0")
endif()

if(ENABLE_INSTRUMENTATION)
   add_definitions("-DENABLE_INSTRUMENTATION=1")
else()
   add_definitions("-DENABLE_INSTRUMENTATION=0")
endif()

target_link_libraries(${PROJECT_NAME} ${LINK_WSI_LIBS})

set(WSI_LAYER_LIBRARY_FILE "${CMAKE_SHARED_LIBRARY_PREFIX}${PROJECT_NAME}${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(WSI_LAYER_MANIFEST_TEMPLATE "${PROJECT_SOURCE_DIR}/layer/VkLayer_window_system_integration.json")
set(WSI_LAYER_MANIFEST_SCRIPT "${PROJECT_SOURCE_DIR}/layer/generate_manifest.cmake")
set(WSI_LAYER_BUILD_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_window_system_integration.json")
set(WSI_LAYER_INSTALL_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/install/VkLayer_window_system_integration.json")
string(REPLACE ";" "|" WSI_MANIFEST_REMOVED_EXTENSIONS_ARG "${WSI_MANIFEST_REMOVED_EXTENSIONS}")

# library_path written in the installed manifest. The default absolute path always works; distribution packages
# may use the bare file name (resolved by the dynamic loader search path), which keeps the manifest identical across
# architectures (required for Multi-Arch: same).
set(WSI_LAYER_MANIFEST_LIBRARY_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/${WSI_LAYER_LIBRARY_FILE}" CACHE STRING
    "library_path of the installed layer manifest")

# The build-tree manifest keeps the relative "./lib..." path so that VK_LAYER_PATH=<build dir> works.
# The installed manifest needs an absolute path: a relative path is resolved against the manifest directory
# (/usr/share/vulkan/...), not against the library directory.
add_custom_command(
   OUTPUT ${WSI_LAYER_BUILD_MANIFEST} ${WSI_LAYER_INSTALL_MANIFEST}
   COMMAND ${CMAKE_COMMAND} -DINPUT=${WSI_LAYER_MANIFEST_TEMPLATE} -DOUTPUT=${WSI_LAYER_BUILD_MANIFEST}
           -DREMOVE_EXTENSIONS=${WSI_MANIFEST_REMOVED_EXTENSIONS_ARG} -P ${WSI_LAYER_MANIFEST_SCRIPT}
   COMMAND ${CMAKE_COMMAND} -DINPUT=${WSI_LAYER_MANIFEST_TEMPLATE} -DOUTPUT=${WSI_LAYER_INSTALL_MANIFEST}
           -DREMOVE_EXTENSIONS=${WSI_MANIFEST_REMOVED_EXTENSIONS_ARG}
           -DLIBRARY_PATH=${WSI_LAYER_MANIFEST_LIBRARY_PATH} -P ${WSI_LAYER_MANIFEST_SCRIPT}
   DEPENDS ${WSI_LAYER_MANIFEST_TEMPLATE} ${WSI_LAYER_MANIFEST_SCRIPT}
   COMMENT "Generating Vulkan layer manifests"
   VERBATIM)
add_custom_target(manifest_json ALL DEPENDS ${WSI_LAYER_BUILD_MANIFEST} ${WSI_LAYER_INSTALL_MANIFEST})

# Installation
install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(WSI_LAYER_INSTALL_IMPLICIT)
   set(WSI_LAYER_MANIFEST_DIR "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d")
else()
   set(WSI_LAYER_MANIFEST_DIR "${CMAKE_INSTALL_DATADIR}/vulkan/explicit_layer.d")
endif()
install(FILES ${WSI_LAYER_INSTALL_MANIFEST} DESTINATION ${WSI_LAYER_MANIFEST_DIR})
