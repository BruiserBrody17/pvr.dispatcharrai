# SPDX-License-Identifier: GPL-2.0-or-later
#
# CoreELEC/LibreELEC out-of-tree package definition for this addon.
# CoreELEC's build harness doesn't support pointing at a live git branch the
# way Kodi's own tools/depends/target/binary-addons harness does (see the
# Linux/macOS/Windows steps in ../../../docs/BUILDING.md) -- every in-tree
# package.mk it ships (confirmed against CoreELEC/CoreELEC's own
# pvr.hts/pvr.iptvsimple) points PKG_URL at a tagged release tarball with a
# pinned PKG_SHA256, so this file follows the same pattern. It is NOT picked
# up automatically; copy or symlink it into a CoreELEC source checkout
# before building -- see the "CoreELEC on an ODROID N2+" section of
# ../../../docs/BUILDING.md for the exact steps, including how to fill in
# PKG_SHA256 once a release is tagged.

PKG_NAME="pvr.dispatcharrai"
PKG_VERSION="1.0.1"
PKG_SHA256="73f876ba284e25ef8da409800bd1aebdfef57a4ac04ce9bcddf94b9c293c75c1"
PKG_REV="1"
PKG_ARCH="any"
PKG_LICENSE="GPL-2.0-or-later"
PKG_SITE="https://github.com/BruiserBrody17/pvr.dispatcharrai"
PKG_URL="https://github.com/BruiserBrody17/pvr.dispatcharrai/archive/${PKG_VERSION}.tar.gz"
# pvr.hts (also curl-based, no extra XML/compression libs) uses exactly this
# depends set; nlohmann-json and pugixml are pulled in by this addon's own
# CMakeLists.txt via FetchContent at configure time instead of CoreELEC's
# own textproc/nlohmann-json and textproc/pugixml packages, matching how the
# Linux/macOS/Windows builds already source them -- one dependency-fetch
# path across every target rather than a CoreELEC-specific one.
PKG_DEPENDS_TARGET="toolchain curl ${MEDIACENTER}:host"
PKG_SECTION=""
PKG_SHORTDESC="pvr.dispatcharrai"
PKG_LONGDESC="Kodi Live TV and DVR client for Dispatcharr"

PKG_IS_ADDON="yes"
PKG_ADDON_TYPE="xbmc.pvrclient"
