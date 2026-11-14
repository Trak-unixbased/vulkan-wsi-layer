/*
 * Copyright (c) 2024, 2026 Arm Limited.
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

/*
 * DMA-BUF heaps implementation of the wsialloc interface.
 *
 * Heap selection (first heap that can be opened wins):
 *   1. $WSIALLOC_DMA_HEAP                       (runtime override, e.g. "system")
 *   2. WSIALLOC_MEMORY_HEAP_NAME                (build option, default "linux,cma")
 *   3. "default_cma_region", "linux,cma", "reserved"
 *      Recent kernels expose the default CMA area as "default_cma_region" (the legacy name depends on the device
 *      tree / CONFIG_DMABUF_HEAPS_CMA_LEGACY), so the name chosen at build time may not exist on the target.
 *
 * Fallback heap: $WSIALLOC_FALLBACK_DMA_HEAP or WSIALLOC_FALLBACK_HEAP_NAME (default "system", empty disables).
 * It is used when no preferred heap exists, and per allocation when the preferred heap fails (typically a CMA area
 * exhausted by several full-resolution swapchains). The Mali GPU and the compositor import through their MMUs, so
 * non-contiguous "system" buffers stay usable; only direct scanout may then be skipped by the compositor.
 */

#include "wsialloc.h"
#include "wsialloc_helpers.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-heap.h>

/**
 * @brief Version of the wsialloc interface we are implementing in this file.
 *
 * This should only be increased when this implementation is updated to match newer versions of wsialloc.h.
 */
#define WSIALLOC_IMPLEMENTATION_VERSION 3

/* Ensure we are implementing the wsialloc version matching the wsialloc.h header we are using. */
#if WSIALLOC_IMPLEMENTATION_VERSION != WSIALLOC_INTERFACE_VERSION
#error "Version mismatch between wsialloc implementation and interface version"
#endif

const uint32_t WSIALLOC_IMPLEMENTATION_VERSION_SYMBOL = WSIALLOC_IMPLEMENTATION_VERSION;

#ifndef WSIALLOC_MEMORY_HEAP_NAME
#define WSIALLOC_MEMORY_HEAP_NAME "linux,cma"
#endif

#ifndef WSIALLOC_FALLBACK_HEAP_NAME
#define WSIALLOC_FALLBACK_HEAP_NAME "system"
#endif

#define DMA_HEAP_DIR "/dev/dma_heap/"

struct wsialloc_allocator
{
   /* File descriptor for the preferred DMA-BUF heap for allocating memory accessible to
    * the windowing system (display, compositor, etc.)
    */
   int memory_fd;

   /* File descriptor for the fallback DMA-BUF heap, or -1. */
   int fallback_fd;

   /* File descriptor for a DMA-BUF heap for allocating protected memory
    * accessible to the windowing system.
    */
   int protected_fd;
};

static int open_heap(const char *name)
{
   if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL)
   {
      return -1;
   }

   char path[PATH_MAX];
   const int len = snprintf(path, sizeof(path), DMA_HEAP_DIR "%s", name);
   if (len < 0 || (size_t)len >= sizeof(path))
   {
      return -1;
   }

   /* O_CLOEXEC: the heap must not leak into processes spawned by the application. */
   return open(path, O_RDONLY | O_CLOEXEC);
}

static int open_preferred_heap(void)
{
   const char *const candidates[] = {
      getenv("WSIALLOC_DMA_HEAP"), WSIALLOC_MEMORY_HEAP_NAME, "default_cma_region", "linux,cma", "reserved",
   };

   for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
   {
      const int fd = open_heap(candidates[i]);
      if (fd >= 0)
      {
         return fd;
      }
   }
   return -1;
}

static int open_fallback_heap(void)
{
   const char *name = getenv("WSIALLOC_FALLBACK_DMA_HEAP");
   if (name == NULL)
   {
      name = WSIALLOC_FALLBACK_HEAP_NAME;
   }
   return open_heap(name);
}

static bool same_heap(int fd_a, int fd_b)
{
   struct stat st_a;
   struct stat st_b;
   if (fstat(fd_a, &st_a) != 0 || fstat(fd_b, &st_b) != 0)
   {
      return false;
   }
   return st_a.st_rdev == st_b.st_rdev;
}

static int allocate(int fd, uint64_t size)
{
   assert(size > 0);
   assert(fd != -1);

   struct dma_heap_allocation_data heap_data = {
      .len = size,
      .fd_flags = O_RDWR | O_CLOEXEC,
   };

   int ret;
   do
   {
      ret = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &heap_data);
   } while (ret != 0 && errno == EINTR);

   if (ret != 0)
   {
      return -errno;
   }
   assert(heap_data.fd > 0);
   return (int)heap_data.fd;
}

static int dma_allocate(const wsialloc_allocator *allocator, const wsialloc_allocate_info *info, uint64_t size)
{
   assert(allocator != NULL);
   assert(info != NULL);
   assert(size > 0);

   /* Protected memory never falls back to an unprotected heap. */
   if (info->flags & WSIALLOC_ALLOCATE_PROTECTED)
   {
      if (allocator->protected_fd < 0)
      {
         return -ENODEV;
      }
      return allocate(allocator->protected_fd, size);
   }

   int fd = allocate(allocator->memory_fd, size);
   if (fd < 0 && allocator->fallback_fd >= 0)
   {
      static bool warned = false;
      if (!warned)
      {
         warned = true;
         fprintf(stderr, "wsialloc: preferred DMA-BUF heap allocation of %llu bytes failed (%d), using fallback heap\n",
                 (unsigned long long)size, fd);
      }
      fd = allocate(allocator->fallback_fd, size);
   }
   return fd;
}

static void close_fd(int fd)
{
   if (fd >= 0)
   {
      close(fd);
   }
}

wsialloc_error wsialloc_new(wsialloc_allocator **allocator)
{
   assert(allocator != NULL);

   wsialloc_allocator *dma_buf_heaps = malloc(sizeof(*dma_buf_heaps));
   if (NULL == dma_buf_heaps)
   {
      return WSIALLOC_ERROR_NO_RESOURCE;
   }

   dma_buf_heaps->memory_fd = open_preferred_heap();
   dma_buf_heaps->fallback_fd = open_fallback_heap();
   dma_buf_heaps->protected_fd = -1;

   if (dma_buf_heaps->memory_fd < 0)
   {
      /* No CMA-like heap at all: allocate everything from the fallback heap. */
      dma_buf_heaps->memory_fd = dma_buf_heaps->fallback_fd;
      dma_buf_heaps->fallback_fd = -1;
   }
   else if (dma_buf_heaps->fallback_fd >= 0 && same_heap(dma_buf_heaps->memory_fd, dma_buf_heaps->fallback_fd))
   {
      close_fd(dma_buf_heaps->fallback_fd);
      dma_buf_heaps->fallback_fd = -1;
   }

   if (dma_buf_heaps->memory_fd < 0)
   {
      fprintf(stderr, "wsialloc: no usable DMA-BUF heap found in " DMA_HEAP_DIR "\n");
      free(dma_buf_heaps);
      return WSIALLOC_ERROR_NO_RESOURCE;
   }

   *allocator = dma_buf_heaps;
   return WSIALLOC_ERROR_NONE;
}

void wsialloc_delete(wsialloc_allocator *allocator)
{
   assert(allocator != NULL);
   if (NULL == allocator)
   {
      return;
   }

   close_fd(allocator->memory_fd);
   close_fd(allocator->fallback_fd);
   close_fd(allocator->protected_fd);

   allocator->memory_fd = -1;
   allocator->fallback_fd = -1;
   allocator->protected_fd = -1;

   free(allocator);
}

wsialloc_error wsialloc_alloc(wsialloc_allocator *allocator, const wsialloc_allocate_info *info,
                              wsialloc_allocate_result *result)
{
   assert(allocator != NULL);
   assert(info != NULL);

   int fd_to_use = allocator->memory_fd;
   if (info->flags & WSIALLOC_ALLOCATE_PROTECTED)
   {
      fd_to_use = allocator->protected_fd;
   }
   if (fd_to_use < 0)
   {
      return WSIALLOC_ERROR_NO_RESOURCE;
   }
   return wsiallocp_alloc(allocator, dma_allocate, info, result);
}
