# Building pvr.dispatcharrai

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

## Linux / macOS (x86_64)

1. Clone a Kodi source tree matching your target Kodi major version (e.g.
   the `Omega` branch for Kodi 21.x, since that's roughly what CoreELEC on
   an ODROID N2+ ships):
   ```bash
   git clone --branch Omega --depth 1 https://github.com/xbmc/xbmc.git kodi-source
   ```
2. Clone this addon next to it (any path):
   ```bash
   git clone https://github.com/BruiserBrody17/pvr.dispatcharrai.git addons/pvr.dispatcharrai
   ```
3. Register the addon with the harness. `pvr.dispatcharrai` isn't in Kodi's
   official addon manifests, so `ADDON_SRC_PREFIX` alone won't find it --
   the harness needs an explicit definition file pointing at the local
   checkout via a `file://` URL:
   ```bash
   mkdir -p addon-defs/pvr.dispatcharrai
   echo "pvr.dispatcharrai file://$(pwd)/addons/pvr.dispatcharrai" \
     > addon-defs/pvr.dispatcharrai/pvr.dispatcharrai.txt
   ```
4. Run the addon build harness. `PREFIX` is required even for a native
   (non-cross-compiling) build -- point it at any writable install
   destination:
   ```bash
   cd kodi-source
   make -j$(nproc) -C tools/depends/target/binary-addons \
     ADDONS="pvr.dispatcharrai" \
     ADDONS_DEFINITION_DIR="$(pwd)/../addon-defs" \
     PREFIX="$(pwd)/../install" \
     EXTRA_CMAKE_ARGS="-DPACKAGE_ZIP=ON" \
     PACKAGE=1
   ```
   On macOS, this same `make` invocation works from a shell with Xcode
   command line tools installed; see the next section for Windows, which
   doesn't have `make` and needs a couple of extra dependency steps. If a
   prior run failed partway through, delete
   `tools/depends/target/binary-addons/.installed-native` first -- the
   harness touches that marker even after a failed configure, which
   otherwise makes it skip reconfiguring on the next run.
5. `-DPACKAGE_DIR` looks like it should redirect where CPack writes the
   zip, but doesn't actually take effect for this harness (confirmed:
   verified against a real run, CPack still wrote it deep inside the
   addon's own ExternalProject build tree regardless of that flag) --
   `PACKAGE_ZIP=ON` alone is what makes CPack build it at all, so find it
   afterward instead of trusting `PACKAGE_DIR`:
   ```bash
   find tools/depends/target/binary-addons -name 'addon-pvr.dispatcharrai-*.zip'
   ```
   That zip (named e.g. `addon-pvr.dispatcharrai-0.1.0-osx-arm64.zip`) is
   what you install via Kodi's
   "install from zip file" option, or publish in a self-hosted repository
   (see the "Distribution" section below).

## Windows (x86_64)

Windows has no `make` and no system libcurl, so this doesn't go through
`tools/depends/target/binary-addons` -- instead it invokes the same
underlying `cmake/addons` project that Makefile wraps, directly, with the
Visual Studio generator. `.github/workflows/build.yml` automates all of
this; the steps below are what that workflow actually runs; every step here
was verified against a real build, not just written from Kodi's docs.

1. Clone Kodi and this addon exactly as in steps 1-2 above (Omega branch,
   this repo checked out under `addons/pvr.dispatcharrai`), and register the
   addon exactly as in step 3.
2. Fetch curl and its own dependencies. Windows has no system libcurl, and
   the prebuilt curl Kodi's own dependency mirror serves needs OpenSSL and
   zlib to link against (confirmed by actually running this build -- curl's
   own `CURLConfig.cmake` pulls both in transitively). Point
   `ADDON_DEPENDS_PATH` at the *same* depends directory the main addon build
   will look in by default (`<BUILD_DIR>/depends`), so one `find_package`
   picks up both Kodi's own generated config and these:
   ```powershell
   $depends = "$pwd\build\build\depends"
   $prebuilt = "kodi-source\cmake\addons\depends\windows\prebuilt"
   "curl http://mirrors.kodi.tv/build-deps/win32/curl-7.67.0-x64-v141-20200105.7z" `
     | Out-File -Encoding ascii "$prebuilt\curl.txt"
   "openssl http://mirrors.kodi.tv/build-deps/win32/openssl-1.1.1q-x64-v142-20221017.7z" `
     | Out-File -Encoding ascii "$prebuilt\openssl.txt"
   "zlib http://mirrors.kodi.tv/build-deps/win32/zlib-1.2.11-x64-v141-20200105.7z" `
     | Out-File -Encoding ascii "$prebuilt\zlib.txt"
   cmake -S kodi-source\cmake\addons\depends\windows -B deps-build -G "Visual Studio 17 2022" -A x64 "-DADDON_DEPENDS_PATH=$depends"
   cmake --build deps-build --config Release
   ```
   Then delete the three `.txt` files you just created. The main build's own
   generic dependency scan (`add_addon_depends`) also globs this same
   `prebuilt/` directory, and having both declare a same-named CMake target
   is a hard configure error -- the files have done their job once the real
   `.lib`/`.dll` files are sitting in `$depends`.
3. Configure and build the addon itself. Quote every `-D` argument as one
   token (`"-DFOO=bar"`, not `-DFOO=bar` split across a line continuation) --
   PowerShell's backtick line continuation can otherwise truncate a value at
   the first `.` it contains, which silently turns `pvr.dispatcharrai` into
   just `pvr`:
   ```powershell
   cmake -S kodi-source\cmake\addons -B build -G "Visual Studio 17 2022" -A x64 `
     "-DADDONS_TO_BUILD=pvr.dispatcharrai" `
     "-DADDONS_DEFINITION_DIR=$pwd\addon-defs" `
     "-DCMAKE_INSTALL_PREFIX=$pwd\install" `
     "-DPACKAGE_ZIP=ON" `
     "-DPACKAGE_DIR=$pwd\dist" `
     "-DBUILD_DIR=$pwd\build\build"
   cmake --build build --config Release --target package-pvr.dispatcharrai
   ```
   Don't pass a custom `-DCMAKE_PREFIX_PATH` here even if you're tempted to
   point it at a separate curl install location: `cmake/addons/CMakeLists.txt`
   builds that variable's final value via an *unquoted* `${CMAKE_PREFIX_PATH}`
   expansion, which silently drops every entry but the first when it's a
   multi-item list. Installing curl straight into the default depends path
   in step 2, and leaving `CMAKE_PREFIX_PATH` alone here, sidesteps that bug
   entirely.
4. The resulting zip in `dist` bundles `libcurl.dll` and `zlib.dll` alongside
   `pvr.dispatcharrai.dll` (see `CMakeLists.txt`'s `DISPATCHARR_ADDITIONAL_BINARY`),
   since a standalone Windows install can't assume those are already present
   the way Kodi's own bundled curl, or Linux's system libcurl, would be.

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
   `/storage/.kodi/addons/pvr.dispatcharrai/` over SSH and restart Kodi. This
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
