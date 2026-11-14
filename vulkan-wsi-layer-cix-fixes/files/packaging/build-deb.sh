#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build a Debian package of the Vulkan WSI layer.
#
# Target: Orange Pi 6 Plus (CIX CD8180 / Sky1, Mali-G720-Immortalis, Arm DDK), Ubuntu 26.04 LTS arm64.
# The script also works on any Debian/Ubuntu host for its native architecture.
#
# Usage: packaging/build-deb.sh [options]
#
#   --version VER         Upstream version (default: derived from git, e.g. 0~git20260615.37817c0)
#   --revision REV        Debian revision (default: 1)
#   --maintainer "N <m>"  Maintainer field (default: from git config, or a placeholder)
#   --output DIR          Where to write the .deb (default: <repo>/dist)
#   --build-dir DIR       Build directory (default: <repo>/build-deb)
#   --jobs N              Parallel jobs (default: nproc)
#   --vulkan-headers DIR  Use these Vulkan headers instead of the system ones (directory containing vulkan/)
#   --heap NAME           Preferred DMA-BUF heap (default: linux,cma; the layer also tries
#                         default_cma_region and reserved, then falls back to "system")
#   --no-xcb              Do not build the X11 (XCB/Xlib) backend
#   --no-wayland          Do not build the Wayland backend
#   --with-display        Also build the VK_KHR_display (DRM/KMS) backend
#   --explicit            Install the manifest as an explicit layer (not loaded automatically)
#   --no-strip            Keep debug symbols in the library
#   --install-deps        Install the build dependencies with apt-get (uses sudo when not root)
#   -h, --help            Show this help
#
# The resulting package installs:
#   /usr/lib/<multiarch>/libVkLayer_window_system_integration.so
#   /usr/share/vulkan/implicit_layer.d/VkLayer_window_system_integration.json
#
# At run time the layer can be disabled with DISABLE_WSI_LAYER=1.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PKG_NAME="vulkan-wsi-layer"
VERSION=""
REVISION="1"
MAINTAINER=""
OUTPUT_DIR="${SRC_DIR}/dist"
BUILD_DIR="${SRC_DIR}/build-deb"
JOBS="$(nproc 2>/dev/null || echo 4)"
VULKAN_HEADERS=""
HEAP_NAME="linux,cma"
WITH_XCB=ON
WITH_WAYLAND=ON
WITH_DISPLAY=OFF
IMPLICIT=ON
STRIP=1
INSTALL_DEPS=0

BUILD_DEPS=(
   build-essential cmake ninja-build pkg-config file dpkg-dev
   libvulkan-dev linux-libc-dev libdrm-dev
   libwayland-dev libwayland-bin wayland-protocols
   libx11-dev libx11-xcb-dev libxcb1-dev libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev libxshmfence-dev
)

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mWARNING:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
usage() { awk 'NR > 2 && /^#/ { sub(/^# ?/, ""); print; next } NR > 2 { exit }' "${BASH_SOURCE[0]}"; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
   case "$1" in
      --version) VERSION="$2"; shift 2 ;;
      --revision) REVISION="$2"; shift 2 ;;
      --maintainer) MAINTAINER="$2"; shift 2 ;;
      --output) OUTPUT_DIR="$2"; shift 2 ;;
      --build-dir) BUILD_DIR="$2"; shift 2 ;;
      --jobs) JOBS="$2"; shift 2 ;;
      --vulkan-headers) VULKAN_HEADERS="$2"; shift 2 ;;
      --heap) HEAP_NAME="$2"; shift 2 ;;
      --no-xcb) WITH_XCB=OFF; shift ;;
      --no-wayland) WITH_WAYLAND=OFF; shift ;;
      --with-display) WITH_DISPLAY=ON; shift ;;
      --explicit) IMPLICIT=OFF; shift ;;
      --no-strip) STRIP=0; shift ;;
      --install-deps) INSTALL_DEPS=1; shift ;;
      -h|--help) usage 0 ;;
      *) warn "Unknown option: $1"; usage 1 ;;
   esac
done

# ---------------------------------------------------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------------------------------------------------
if [[ ${INSTALL_DEPS} -eq 1 ]]; then
   SUDO=""
   [[ ${EUID} -ne 0 ]] && SUDO="sudo"
   log "Installing build dependencies"
   ${SUDO} apt-get update
   ${SUDO} env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${BUILD_DEPS[@]}"
fi

missing=()
for tool in cmake pkg-config dpkg-deb dpkg-architecture dpkg-shlibdeps file strip; do
   command -v "${tool}" >/dev/null 2>&1 || missing+=("${tool}")
done
[[ ${#missing[@]} -eq 0 ]] || die "Missing tools: ${missing[*]} (run with --install-deps)"

pc_modules=(libdrm)
[[ ${WITH_WAYLAND} == ON ]] && pc_modules+=(wayland-client wayland-protocols)
[[ ${WITH_XCB} == ON ]] && pc_modules+=(xcb xcb-dri3 xcb-present xcb-sync xshmfence x11-xcb)
[[ -z ${VULKAN_HEADERS} ]] && pc_modules+=(vulkan)
for module in "${pc_modules[@]}"; do
   pkg-config --exists "${module}" || die "pkg-config module '${module}' not found (run with --install-deps)"
done
[[ -f /usr/include/linux/dma-heap.h ]] || die "linux/dma-heap.h not found (install linux-libc-dev)"

# Vulkan header version check (the layer is validated against >= 1.4.299).
if [[ -n ${VULKAN_HEADERS} ]]; then
   VK_CORE_H="${VULKAN_HEADERS}/vulkan/vulkan_core.h"
else
   VK_CORE_H="$(pkg-config --variable=includedir vulkan)/vulkan/vulkan_core.h"
fi
[[ -f ${VK_CORE_H} ]] || die "vulkan_core.h not found (${VK_CORE_H})"
VK_HEADER_VERSION="$(sed -n 's/^#define VK_HEADER_VERSION \([0-9]\+\).*/\1/p' "${VK_CORE_H}" | head -n1)"
log "Vulkan headers: 1.x.${VK_HEADER_VERSION} (${VK_CORE_H})"
if [[ -n ${VK_HEADER_VERSION} && ${VK_HEADER_VERSION} -lt 299 ]]; then
   warn "Vulkan headers older than 1.4.299, the build may fail. Use --vulkan-headers or Ubuntu >= 25.04."
fi

# ---------------------------------------------------------------------------------------------------------------------
# Package metadata
# ---------------------------------------------------------------------------------------------------------------------
DEB_ARCH="$(dpkg-architecture -qDEB_HOST_ARCH)"
MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"

if [[ -z ${VERSION} ]]; then
   if git -C "${SRC_DIR}" rev-parse --git-dir >/dev/null 2>&1; then
      commit_date="$(git -C "${SRC_DIR}" log -1 --format=%cd --date=format:%Y%m%d)"
      commit_sha="$(git -C "${SRC_DIR}" rev-parse --short=7 HEAD)"
      VERSION="0~git${commit_date}.${commit_sha}"
      if ! git -C "${SRC_DIR}" diff --quiet HEAD -- 2>/dev/null; then
         VERSION="${VERSION}+local"
      fi
   else
      VERSION="0~$(date +%Y%m%d)"
   fi
fi
[[ ${VERSION} =~ ^[0-9][A-Za-z0-9.+~]*$ ]] || die "Invalid Debian version: ${VERSION}"
FULL_VERSION="${VERSION}-${REVISION}"

if [[ -z ${MAINTAINER} ]]; then
   git_name="$(git -C "${SRC_DIR}" config user.name 2>/dev/null || true)"
   git_mail="$(git -C "${SRC_DIR}" config user.email 2>/dev/null || true)"
   if [[ -n ${git_name} && -n ${git_mail} ]]; then
      MAINTAINER="${git_name} <${git_mail}>"
   else
      MAINTAINER="Local Build <root@localhost>"
   fi
fi

if [[ ${IMPLICIT} == ON ]]; then
   MANIFEST_DIR="usr/share/vulkan/implicit_layer.d"
else
   MANIFEST_DIR="usr/share/vulkan/explicit_layer.d"
fi

log "Package ${PKG_NAME} ${FULL_VERSION} (${DEB_ARCH}, ${MULTIARCH})"
log "Backends: headless=ON wayland=${WITH_WAYLAND} xcb=${WITH_XCB} display=${WITH_DISPLAY}, heap=${HEAP_NAME}"

# ---------------------------------------------------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------------------------------------------------
GENERATOR=()
command -v ninja >/dev/null 2>&1 && GENERATOR=(-G Ninja)

CMAKE_ARGS=(
   -S "${SRC_DIR}" -B "${BUILD_DIR}" "${GENERATOR[@]}"
   -DCMAKE_BUILD_TYPE=Release
   -DCMAKE_INSTALL_PREFIX=/usr
   -DCMAKE_INSTALL_LIBDIR="lib/${MULTIARCH}"
   -DBUILD_WSI_HEADLESS=ON
   -DBUILD_WSI_WAYLAND="${WITH_WAYLAND}"
   -DBUILD_WSI_XCB="${WITH_XCB}"
   -DBUILD_WSI_DISPLAY="${WITH_DISPLAY}"
   -DSELECT_EXTERNAL_ALLOCATOR=dma_buf_heaps
   -DWSIALLOC_MEMORY_HEAP_NAME="${HEAP_NAME}"
   -DWSIALLOC_FALLBACK_HEAP_NAME=system
   -DWSI_LAYER_INSTALL_IMPLICIT="${IMPLICIT}"
   -DWSI_LAYER_MANIFEST_LIBRARY_PATH=libVkLayer_window_system_integration.so
   -DVULKAN_WSI_LAYER_EXPERIMENTAL=OFF
)
[[ -n ${VULKAN_HEADERS} ]] && CMAKE_ARGS+=(-DVULKAN_CXX_INCLUDE="${VULKAN_HEADERS}")

# Debian hardening flags (bindnow, relro, fortify, stack protector, branch protection on arm64).
if command -v dpkg-buildflags >/dev/null 2>&1; then
   export DEB_BUILD_MAINT_OPTIONS="hardening=+all"
   CMAKE_ARGS+=(
      -DCMAKE_C_FLAGS="$(dpkg-buildflags --get CFLAGS) $(dpkg-buildflags --get CPPFLAGS)"
      -DCMAKE_CXX_FLAGS="$(dpkg-buildflags --get CXXFLAGS) $(dpkg-buildflags --get CPPFLAGS)"
      -DCMAKE_SHARED_LINKER_FLAGS="$(dpkg-buildflags --get LDFLAGS)"
   )
fi

log "Configuring"
rm -rf "${BUILD_DIR}"
cmake "${CMAKE_ARGS[@]}"

log "Building (${JOBS} jobs)"
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"

# ---------------------------------------------------------------------------------------------------------------------
# Staging
# ---------------------------------------------------------------------------------------------------------------------
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
PKG_ROOT="${STAGE}/${PKG_NAME}_${FULL_VERSION}_${DEB_ARCH}"

log "Installing into staging area"
DESTDIR="${PKG_ROOT}" cmake --install "${BUILD_DIR}"

LIB_PATH="${PKG_ROOT}/usr/lib/${MULTIARCH}/libVkLayer_window_system_integration.so"
MANIFEST_PATH="${PKG_ROOT}/${MANIFEST_DIR}/VkLayer_window_system_integration.json"
[[ -f ${LIB_PATH} ]] || die "Layer library not installed (${LIB_PATH})"
[[ -f ${MANIFEST_PATH} ]] || die "Layer manifest not installed (${MANIFEST_PATH})"

# The manifest references the library by file name: the Vulkan loader then resolves it through the dynamic linker
# search path (/usr/lib/<multiarch>), which keeps the manifest identical across architectures (Multi-Arch: same).
grep -Eq '"library_path"[[:space:]]*:[[:space:]]*"libVkLayer_window_system_integration\.so"' "${MANIFEST_PATH}" \
   || die "Unexpected library_path in ${MANIFEST_PATH}"

if [[ ${STRIP} -eq 1 ]]; then
   strip --strip-unneeded --remove-section=.comment "${LIB_PATH}"
fi
chmod 0644 "${LIB_PATH}" "${MANIFEST_PATH}"

# Documentation
DOC_DIR="${PKG_ROOT}/usr/share/doc/${PKG_NAME}"
install -d -m 0755 "${DOC_DIR}"
install -m 0644 "${SRC_DIR}/README.md" "${DOC_DIR}/README.md"
gzip -9n "${DOC_DIR}/README.md"

cat > "${DOC_DIR}/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: vulkan-wsi-layer
Source: https://github.com/Trak-unixbased/vulkan-wsi-layer
 (fork of https://gitlab.freedesktop.org/mesa/vulkan-wsi-layer)

Files: *
Copyright: 2017-2026 Arm Limited
License: MIT

Files: wsi/xcb/*
Copyright: 2026 Cix Technology Group Co., Ltd.
           2017-2022 Arm Limited
License: Apache-2.0 and MIT

License: MIT
$(sed -n '/Permission is hereby granted/,/SOFTWARE\./p' "${SRC_DIR}/LICENSE" | sed 's/^/ /; s/^ $/ ./')

License: Apache-2.0
 On Debian systems, the full text of the Apache License, Version 2.0 can be found in
 /usr/share/common-licenses/Apache-2.0.
EOF
chmod 0644 "${DOC_DIR}/copyright"

DATE_RFC2822="$(LC_ALL=C date -R)"
if git -C "${SRC_DIR}" rev-parse --git-dir >/dev/null 2>&1; then
   DATE_RFC2822="$(LC_ALL=C git -C "${SRC_DIR}" log -1 --format=%cD)"
fi
cat > "${DOC_DIR}/changelog.Debian" <<EOF
${PKG_NAME} (${FULL_VERSION}) unstable; urgency=medium

  * Local build for Orange Pi 6 Plus (CIX CD8180, Mali-G720),
    Ubuntu 26.04 LTS.

 -- ${MAINTAINER}  ${DATE_RFC2822}
EOF
gzip -9n "${DOC_DIR}/changelog.Debian"

# ---------------------------------------------------------------------------------------------------------------------
# Control files
# ---------------------------------------------------------------------------------------------------------------------
log "Computing shared library dependencies"
SHLIBS_WORK="${STAGE}/shlibs"
mkdir -p "${SHLIBS_WORK}/debian"
cat > "${SHLIBS_WORK}/debian/control" <<EOF
Source: ${PKG_NAME}

Package: ${PKG_NAME}
Architecture: ${DEB_ARCH}
EOF
SHLIBS_DEPENDS="$(cd "${SHLIBS_WORK}" && dpkg-shlibdeps -O -e"${LIB_PATH}" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')"
[[ -n ${SHLIBS_DEPENDS} ]] || die "dpkg-shlibdeps did not return any dependency"

# The layer is loaded by the Vulkan loader; libvulkan1 is not linked but required at run time.
DEPENDS="${SHLIBS_DEPENDS}, libvulkan1 (>= 1.3.0)"
INSTALLED_SIZE="$(du -sk --exclude=DEBIAN "${PKG_ROOT}" | cut -f1)"

install -d -m 0755 "${PKG_ROOT}/DEBIAN"
cat > "${PKG_ROOT}/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${FULL_VERSION}
Architecture: ${DEB_ARCH}
Maintainer: ${MAINTAINER}
Installed-Size: ${INSTALLED_SIZE}
Depends: ${DEPENDS}
Section: libs
Priority: optional
Multi-Arch: same
Homepage: https://gitlab.freedesktop.org/mesa/vulkan-wsi-layer
Description: Vulkan window system integration layer (Arm Mali / CIX)
 Vulkan layer implementing VK_KHR_swapchain and the surface extensions
 (Wayland, X11 XCB/Xlib, headless) for Vulkan drivers that do not provide
 their own window system integration, such as the Arm Mali DDK shipped for
 the CIX CD8180 (Sky1) SoC of the Orange Pi 6 Plus.
 .
 The layer is installed as an implicit layer and is enabled automatically.
 Set DISABLE_WSI_LAYER=1 to disable it, WSI_LAYER_DEVICE_FILTER=all to
 expose non-Mali GPUs, and WSIALLOC_DMA_HEAP to select the DMA-BUF heap.
EOF

# Shared library in a standard library directory: refresh the ld.so cache.
echo "activate-noawait ldconfig" > "${PKG_ROOT}/DEBIAN/triggers"

# md5sums of every shipped file
(cd "${PKG_ROOT}" && find . -path ./DEBIAN -prune -o -type f -print0 | LC_ALL=C sort -z | xargs -0 md5sum \
   | sed 's| \./| |') > "${PKG_ROOT}/DEBIAN/md5sums"
chmod 0644 "${PKG_ROOT}/DEBIAN/control" "${PKG_ROOT}/DEBIAN/md5sums" "${PKG_ROOT}/DEBIAN/triggers"

# Normalise permissions (umask independent).
find "${PKG_ROOT}" -type d -exec chmod 0755 {} +

# ---------------------------------------------------------------------------------------------------------------------
# Build the package
# ---------------------------------------------------------------------------------------------------------------------
mkdir -p "${OUTPUT_DIR}"
DEB_FILE="${OUTPUT_DIR}/${PKG_NAME}_${FULL_VERSION}_${DEB_ARCH}.deb"
log "Building ${DEB_FILE}"
dpkg-deb --root-owner-group -Zxz --build "${PKG_ROOT}" "${DEB_FILE}" >/dev/null

if command -v lintian >/dev/null 2>&1; then
   log "lintian (informative)"
   lintian --no-tag-display-limit --suppress-tags no-manual-page,initial-upload-closes-no-bugs,package-name-doesnt-match-sonames,shared-library-lacks-version \
      "${DEB_FILE}" || true
fi

log "Contents"
dpkg-deb --info "${DEB_FILE}" | sed -n '/Package:/,$p'
dpkg-deb --contents "${DEB_FILE}"

cat <<EOF

Package ready: ${DEB_FILE}

Install on the Orange Pi 6 Plus:
  sudo apt install ./$(basename "${DEB_FILE}")

Check:
  vulkaninfo --summary          # the layer appears in "Instance Layers"
  vkcube --wsi wayland          # or: vkcube --wsi xcb (X11 / Xwayland)
EOF
