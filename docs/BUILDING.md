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

The `-G "Visual Studio ..."` generator string below must match whichever
Visual Studio version is actually installed -- CMake doesn't fall back to
whatever's present, it fails outright ("could not find any instance of
Visual Studio") if you name a version that isn't there. This has already
bitten CI once: GitHub's `windows-latest` runner moved to shipping only VS
2026 (`"Visual Studio 18 2026"`) with no VS2022 left on the image at all,
confirmed by running `vswhere -all -format json` in the workflow. A local
machine may well still have VS2022 (`"Visual Studio 17 2022"`) instead --
check what you actually have (Visual Studio Installer, or `vswhere -all`)
rather than assuming either one.

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
4. `-DPACKAGE_DIR` doesn't actually redirect CPack's output here either (same
   as the Linux/macOS harness above) -- confirmed via a real run's CPack log,
   it instead drops the zip in the OS temp dir (`$env:TEMP`, e.g.
   `addon-pvr.dispatcharrai-0.1.0-windows-x86_64.zip` under
   `C:\Users\<runner>\AppData\Local\Temp`). Find it there rather than in
   `dist`:
   ```powershell
   Get-ChildItem -Path $env:TEMP -Filter 'addon-pvr.dispatcharrai-*.zip' -Recurse
   ```
   That zip bundles `libcurl.dll` and `zlib.dll` alongside
   `pvr.dispatcharrai.dll` (see `CMakeLists.txt`'s `DISPATCHARR_ADDITIONAL_BINARY`),
   since a standalone Windows install can't assume those are already present
   the way Kodi's own bundled curl, or Linux's system libcurl, would be.

## CoreELEC on an ODROID N2+ (aarch64 / Amlogic S922X)

This is the part that most often causes "it built but Kodi won't load it"
problems, because CoreELEC ships its own cross-compiled Kodi build with a
specific compiler/libc/Kodi-commit combination -- a binary built against a
generic Linux aarch64 toolchain will very likely **not** be ABI-compatible,
even though it's technically the same architecture.

A GitHub Actions job for this was considered and rejected: CoreELEC's own
build harness (a LibreELEC fork) is designed around persistent, self-hosted
build infrastructure -- confirmed by reading LibreELEC's own addon-build
automation (`LibreELEC/actions` on GitHub), which runs on
`[self-hosted, nightly]` runners with a 12-hour timeout and host-mounted
`/sources`, `/target`, `/build-root` directories that persist across runs,
specifically so the cross toolchain and every dependency aren't rebuilt
from scratch every time. GitHub's hosted runners are ephemeral, capped at
6 hours, and give ~14GB free disk -- a real mismatch for a from-scratch
build, not just a "might be slow" one. Building locally is the practical
path.

### Building it as a CoreELEC package (recommended)

CoreELEC is a LibreELEC-derived buildroot distro; third-party binary Kodi
addons are added as a package under
`packages/mediacenter/kodi-binary-addons/<addon-name>/package.mk` in a
CoreELEC source checkout, following the same pattern as addons they
already ship. Unlike Kodi's own `tools/depends/target/binary-addons`
harness (used above), this doesn't support pointing at a live git branch --
every in-tree example (confirmed by reading CoreELEC/CoreELEC's real
`pvr.hts` and `pvr.iptvsimple` package.mk files) points `PKG_URL` at a
tagged release tarball with a pinned `PKG_SHA256`. A package definition for
this addon following that same pattern is checked in at
[`packaging/coreelec/pvr.dispatcharrai/package.mk`](../packaging/coreelec/pvr.dispatcharrai/package.mk).

1. Tag and publish a GitHub release matching the version in
   `pvr.dispatcharrai/addon.xml.in` (e.g. `0.1.0`, no `v` prefix -- CoreELEC's
   own package.mk files use the tag name verbatim in the archive URL, and
   keeping it identical to `PKG_VERSION` avoids a mismatch):
   ```bash
   git tag 0.1.0
   git push origin 0.1.0
   gh release create 0.1.0 --generate-notes
   ```
2. Compute the tarball's checksum and fill it into
   `packaging/coreelec/pvr.dispatcharrai/package.mk`'s `PKG_SHA256`
   (it currently holds a placeholder of zeros):
   ```bash
   curl -L https://github.com/BruiserBrody17/pvr.dispatcharrai/archive/0.1.0.tar.gz | sha256sum
   ```
3. On a Debian/Ubuntu-based Linux build machine (not the N2+ itself --
   cross-compiling needs real CPU and disk; per CoreELEC's own build docs,
   budget 50GB free disk and expect the first build to take a while, since
   it also has to fetch/build the aarch64 cross toolchain), clone CoreELEC
   at the branch matching your device's installed CoreELEC major version
   (`coreelec-22` is current as of this writing) and copy the package.mk in:
   ```bash
   git clone --branch coreelec-22 --depth 1 https://github.com/CoreELEC/CoreELEC.git
   mkdir -p CoreELEC/packages/mediacenter/kodi-binary-addons/pvr.dispatcharrai
   cp packaging/coreelec/pvr.dispatcharrai/package.mk \
     CoreELEC/packages/mediacenter/kodi-binary-addons/pvr.dispatcharrai/
   ```
4. Build it. The N2+'s S922X is confirmed (by reading CoreELEC's own source
   tree -- `projects/Amlogic-ce/devices/Amlogic-no` is the directory that
   ships `Odroid_N2_boot.ini`) to fall under the `Amlogic-ce` project,
   `Amlogic-no` device, `aarch64` arch:
   ```bash
   cd CoreELEC
   PROJECT=Amlogic-ce ARCH=aarch64 DEVICE=Amlogic-no ./scripts/create_addon pvr.dispatcharrai
   ```
   Per CoreELEC's own wiki, the result lands under `target/addons/`; this
   hasn't been verified against a real run, so if it's not there, search
   `target/` for `pvr.dispatcharrai*.zip` instead.
5. Install the resulting zip on the N2+ via Kodi's "install from zip file"
   option, or copy it over SSH and extract into
   `/storage/.kodi/addons/pvr.dispatcharrai/` directly, then restart Kodi.

For later releases, only steps 1-2 and the `cp`/build in steps 3-4 need
repeating -- the CoreELEC checkout itself can be reused.

### Manual cross-compile + side-load (fallback)

If you'd rather not maintain a package.mk (e.g. testing an unreleased
commit), cross-compile using the same toolchain CoreELEC's build system
uses for the N2+ (`Amlogic-ce`/`Amlogic-no`, aarch64), matching their exact
GCC version and the Kodi commit their release is built from, then copy the
resulting `.so` and a rendered `addon.xml` into
`/storage/.kodi/addons/pvr.dispatcharrai/` over SSH and restart Kodi. This
is more fragile than the package.mk route above (nothing pins the toolchain
version for you), so prefer that unless you specifically need to test a
build that isn't tagged.

Either way, pin the Kodi source tree/version you build against to the same
major version CoreELEC's current release ships, or the addon will fail to
load with an API-version mismatch even if the library itself loads fine.

## Distribution (Windows/macOS, once built)

Kodi installs binary addons either as a manual zip, or from a self-hosted
repository (a small `repository.xml`-style addon whose own zip contains an
`addons.xml` index pointing at your built zips, served over `https://` or
GitHub Pages). Claude Code can generate that repository structure once the
desktop builds above are working, if you want "install from repository"
rather than "install from zip file."
