# Building pvr.dispatcharr

Kodi binary addons aren't built standalone in the usual CMake sense --
they're built through Kodi's own addon build harness so the resulting
`.so`/`.dll`/`.dylib` links against exactly the right Kodi ABI. This repo's
`CMakeLists.txt` supplies the addon side of that (`find_package(Kodi
REQUIRED)` + `build_addon(...)`); the harness lives in Kodi's own source
tree at `tools/depends/target/binary-addons`.

This needs real network access (to clone Kodi, fetch nlohmann/json and
pugixml via CMake's `FetchContent`, and pull system libcurl) that this chat
sandbox doesn't have -- do the actual build with **Claude Code** or your own
machine/CI. The GitHub Actions workflow in `.github/workflows/build.yml`
automates the Windows/macOS/Linux steps below on every push.

## Windows / macOS / Linux (x86_64)

1. Clone a Kodi source tree matching your target Kodi major version (e.g.
   the `Omega` branch for Kodi 21.x, since that's roughly what CoreELEC on
   an ODROID N2+ ships):
   ```bash
   git clone --branch Omega --depth 1 https://github.com/xbmc/xbmc.git kodi-source
   ```
2. Clone this addon next to it (any path; `ADDON_SRC_PREFIX` below points at
   its parent directory):
   ```bash
   git clone https://github.com/YOUR-GITHUB-USER/pvr.dispatcharr.git addons/pvr.dispatcharr
   ```
3. Run the addon build harness:
   ```bash
   cd kodi-source
   make -j$(nproc) -C tools/depends/target/binary-addons \
     ADDONS="pvr.dispatcharr" \
     ADDON_SRC_PREFIX="$(pwd)/../addons" \
     EXTRA_CMAKE_ARGS="-DPACKAGE_ZIP=ON -DPACKAGE_DIR=$(pwd)/../dist" \
     PACKAGE=1
   ```
   On Windows, use Kodi's documented CMake/Visual Studio flow instead of
   `make`; on macOS, the same `make` invocation works from a shell with
   Xcode command line tools installed.
4. The resulting zip in `../dist` is what you install via Kodi's
   "install from zip file" option, or publish in a self-hosted repository
   (see the "Distribution" section below).

## CoreELEC on an ODROID N2+ (aarch64 / Amlogic S922X)

This is the part that most often causes "it built but Kodi won't load it"
problems, because CoreELEC ships its own cross-compiled Kodi build with a
specific compiler/libc/Kodi-commit combination -- a binary built against a
generic Linux aarch64 toolchain will very likely **not** be ABI-compatible,
even though it's technically the same architecture.

There are two realistic paths, in order of how "supported" they are:

1. **Build it as a CoreELEC package** (recommended). CoreELEC is a
   LibreELEC-derived buildroot distro; third-party binary Kodi addons are
   normally added as a package under
   `packages/mediacenter/kodi-binary-addons/<addon-name>/package.mk` in a
   fork of the CoreELEC/LibreELEC source tree, following the same pattern
   as addons they already ship (e.g. `pvr.iptvsimple`, `pvr.hts`). That
   package.mk fetches this repo at a pinned commit/tag and runs the exact
   same `tools/depends/target/binary-addons` harness above, but inside
   CoreELEC's own cross-compilation environment, so the ABI matches
   automatically. Building a full CoreELEC image is not required -- their
   build scripts can build a single package/addon target. Ask in the
   CoreELEC forum/Discord which package to model this on for your specific
   CoreELEC release branch; the exact `package.mk` syntax drifts between
   CoreELEC versions and isn't reproduced here to avoid giving you a
   confidently-wrong file.
2. **Manual cross-compile + side-load** (community workaround, more
   fragile). Cross-compile using the same toolchain CoreELEC's build system
   uses for your device's SoC (Amlogic ng, for the N2+), matching their
   exact GCC version and the Kodi commit their release is built from, then
   copy the resulting `.so` and a rendered `addon.xml` into
   `/storage/.kodi/addons/pvr.dispatcharr/` over SSH and restart Kodi. This
   is what enthusiasts typically do for out-of-tree PVR addons on
   LibreELEC/CoreELEC when a package doesn't exist yet -- there's an open
   CoreELEC forum thread asking about exactly this for Dispatcharr, with no
   working answer posted as of this writing, which suggests option 1 is the
   more promising route for a device you plan to rely on daily.

Either way, pin the Kodi source tree you build against (step 1 above) to
the same major version CoreELEC's current release ships, or the addon will
fail to load with an API-version mismatch even if the library itself loads
fine.

## Distribution (Windows/macOS, once built)

Kodi installs binary addons either as a manual zip, or from a self-hosted
repository (a small `repository.xml`-style addon whose own zip contains an
`addons.xml` index pointing at your built zips, served over `https://` or
GitHub Pages). Claude Code can generate that repository structure once the
desktop builds above are working, if you want "install from repository"
rather than "install from zip file."
