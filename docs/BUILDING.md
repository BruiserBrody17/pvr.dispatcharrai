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

**Confirmed live on a real Rocky Linux 10 install (0.4.0):** the steps
above work as documented -- gcc 14/cmake 3.31 from the distro's own repos
built it cleanly, no compatibility issues, once `libcurl-devel` was
installed (the one dependency not present by default). Kodi itself
wasn't packaged for RHEL10/Rocky10 yet at the time (too new for RPM
Fusion); the official Kodi Flatpak (`flatpak install flathub
tv.kodi.Kodi`) worked as a real, officially-supported alternative --
confirmed the natively-built `.so` above loads and runs correctly inside
that Flatpak's sandboxed runtime with no ABI issues.

**A real, non-obvious gotcha if launching Kodi non-interactively (e.g.
over SSH) rather than from the desktop's own app launcher: Kodi's own
process crashed on startup** (`std::vector<...>::front() [CpuData]:
Assertion '!this->empty()' failed`, inside Kodi's own CPU-monitoring
code, before this addon was even instantiated) **when launched without
the logged-in session's actual display environment.** `ssh user@host
'flatpak run tv.kodi.Kodi'` doesn't inherit `WAYLAND_DISPLAY`/`DISPLAY`/
`DBUS_SESSION_BUS_ADDRESS` from the real graphical session at all --
confirmed fixed by pulling those from an already-running process that
*is* part of that session (e.g. `tr '\0' '\n' < /proc/<gnome-shell
pid>/environ`) and exporting them (along with `XDG_RUNTIME_DIR`) before
the `flatpak run` call. Once launched with the correct display
environment, Kodi ran completely stably -- this was an environmental
launch-context issue, not a bug in Kodi or this addon.

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

## CoreELEC on an ODROID N2+ (Amlogic S922X)

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

**This whole path -- package.mk, checksum, branch, device, arch -- has now
been confirmed by an actual successful cross-compile** (`0.3.0`, against a
real WSL2/Ubuntu build machine): `ALL ADDONS BUILT SUCCESSFULLY`, producing
a genuine `pvr.dispatcharrai-0.3.0.1.zip` whose `.so` is a real stripped
ELF binary, and every specific claim below (branch, device, arch, output
path) reflects what that run actually did, not documentation guesswork.
Several real problems surfaced and were fixed along the way -- they're
called out inline so a future rebuild doesn't have to rediscover them.

1. Tag and publish a GitHub release matching the version in
   `pvr.dispatcharrai/addon.xml.in` (e.g. `0.4.0`, no `v` prefix -- CoreELEC's
   own package.mk files use the tag name verbatim in the archive URL, and
   keeping it identical to `PKG_VERSION` avoids a mismatch):
   ```bash
   git tag 0.4.0
   git push origin 0.4.0
   gh release create 0.4.0 --generate-notes
   ```
   **The GitHub repo must be public.** CoreELEC's build harness fetches
   `PKG_URL` with a plain anonymous `curl`/`wget` -- no auth, no token --
   so a private repo's archive URL 404s (GitHub's standard "hide
   existence" response to an unauthenticated request against a private
   repo). Confirmed live: this addon's own repo was still private from an
   earlier session, the cross-compile's very last step
   (`pvr.dispatcharrai:target`, after the entire toolchain had already
   built successfully) failed on exactly this 404, and switching the repo
   to public (`gh repo edit <repo> --visibility public
   --accept-visibility-change-consequences`) immediately fixed it -- no
   package.mk or build-harness change needed.
2. Compute the tarball's checksum and fill it into
   `packaging/coreelec/pvr.dispatcharrai/package.mk`'s `PKG_SHA256`:
   ```bash
   curl -L https://github.com/BruiserBrody17/pvr.dispatcharrai/archive/0.3.0.tar.gz | sha256sum
   ```
   Do this *after* the repo is public, against the real tarball -- doing
   it while the repo is still private silently hashes GitHub's 404 error
   page instead (confirmed live: this is exactly what happened building
   `0.3.0`, and the build failed with an explicit
   `Incorrect checksum calculated on downloaded file` once the repo was
   made public and the real tarball could finally be fetched, until the
   checksum was recomputed against the real content).
3. On a Debian/Ubuntu-based Linux build machine (not the N2+ itself --
   cross-compiling needs real CPU and disk; per CoreELEC's own build docs,
   budget 50GB free disk and expect the first build to take a while, since
   it also has to fetch/build the whole cross toolchain including GCC from
   source), clone CoreELEC at the branch matching your device's installed
   CoreELEC major version -- **use the branch matching CoreELEC's current
   *stable* release, not whatever branch happens to be default/newest**:
   `coreelec-22` tracks Kodi 22 "Piers", which as of this writing is still
   a beta (`22.0b2`) with a materially different PVR addon API (the
   recorded-stream methods gained a `streamId` parameter for
   concurrent-stream support) -- confirmed live by actually
   cross-compiling against it, which failed with `override` mismatches on
   `GetChannelStreamProperties`/`OpenRecordedStream`/
   `CloseRecordedStream`/`ReadRecordedStream`/`SeekRecordedStream`/
   `LengthRecordedStream`, all six exactly matching the API's real,
   deliberate signature change, not a bug in this addon's code. CoreELEC's
   actual latest stable release is `21.3-Omega` (confirmed against
   CoreELEC's own GitHub releases), tracking Kodi 21/Omega -- the same
   branch this addon's Windows/macOS/Linux CI already targets
   (`KODI_BRANCH` in `.github/workflows/build.yml`) and what a real device
   almost certainly runs unless deliberately flashed onto a dev/nightly
   build. This is the branch that actually built successfully:
   ```bash
   git clone --branch coreelec-21 --depth 1 https://github.com/CoreELEC/CoreELEC.git
   mkdir -p CoreELEC/packages/mediacenter/kodi-binary-addons/pvr.dispatcharrai
   cp packaging/coreelec/pvr.dispatcharrai/package.mk \
     CoreELEC/packages/mediacenter/kodi-binary-addons/pvr.dispatcharrai/
   ```
4. Build it. Both the device name *and* the arch value have changed across
   CoreELEC branches -- don't assume either is stable across branches, and
   don't assume the SoC being 64-bit means the build's `ARCH` is
   `aarch64`:
   - **Device**: confirm by reading the target branch's own source tree
     for which device directory under `projects/Amlogic-ce/devices/` ships
     `Odroid_N2_boot.ini`. On `coreelec-21` it's `Amlogic-ng` (confirmed
     live -- `coreelec-22`, which this doc no longer recommends, instead
     used a single combined `Amlogic-no` device that doesn't exist on
     `coreelec-21`).
   - **Arch**: CoreELEC's own official `21.3-Omega` release for this
     device is literally named
     `CoreELEC-Amlogic-ng.arm-21.3-Omega-Odroid_N2.img.gz` (confirmed
     against CoreELEC's own GitHub releases) -- `arm`, not `aarch64`,
     despite the S922X being a 64-bit SoC: CoreELEC's `Amlogic-ng` device
     on the Omega branch runs a 32-bit (`armhf`/EABI5) userland on a
     64-bit kernel, not a true AArch64 userland. Passing `ARCH=aarch64`
     doesn't fail outright -- the build harness silently resolves the
     actual target to `arm` regardless (confirmed by the resulting build
     directory being named `...arm-21`, not `...aarch64-21`, and by the
     finished `.so` being `ELF 32-bit LSB shared object, ARM, EABI5`, not
     64-bit) -- but passing `ARCH=arm` explicitly matches reality and
     avoids the confusion:
   ```bash
   cd CoreELEC
   PROJECT=Amlogic-ce ARCH=arm DEVICE=Amlogic-ng ./scripts/create_addon pvr.dispatcharrai
   ```
   Two more real, confirmed-live failure modes to expect and how they were
   fixed, in case a future build hits them again (both are about *fetching
   dependency source tarballs*, not about this addon's own code):
   - **GNU mirror unreachability**: `ftpmirror.gnu.org` (and CoreELEC's
     own mirrors, `sources.coreelec.org` / `sources.libreelec.tv`) were
     unreachable from the network this was built on, failing `make`,
     `m4`, `autoconf`, `autoconf-archive`, `automake`, `bison`,
     `libtool`, `mpc`, `mpfr`, `readline`, `gcc`, and `gmp` in turn (each
     one only discovered by retrying after the previous one was fixed).
     `ftp.gnu.org` (the canonical, non-redirector GNU host) worked
     throughout. If this happens again: fetch the exact `PKG_VERSION`
     tarball from `https://ftp.gnu.org/gnu/<pkg>/...` (or, for `gmp`,
     `https://ftp.gnu.org/gnu/gmp/...`, since `gmp` doesn't use the
     `ftpmirror.gnu.org` host in its own `package.mk` but is served by
     the same broken `sources.coreelec.org`/`sources.libreelec.tv`
     mirrors as a fallback), verify it against that package's own
     `PKG_SHA256` in `packages/.../<pkg>/package.mk`, then drop it into
     `sources/<pkg>/<pkg>-<version>.<ext>` in the CoreELEC checkout
     alongside two matching sidecar files, `<same filename>.sha256`
     (just the hex digest) and `<same filename>.url` (the original
     `PKG_URL`) -- the build script skips fetching anything it finds
     already staged there with a matching checksum.
   - **`repo.or.cz` bot wall**: `rtmpdump`'s pinned git-snapshot URL
     (`repo.or.cz/rtmpdump.git/snapshot/<commit>.tar.gz`) returned an
     Anubis bot-check HTML page instead of the real tarball, from a plain
     `curl`/`wget` with no way to pass the JS challenge nor via a
     browser-like `User-Agent` header. The Wayback Machine had a genuine
     cached copy from before the bot wall existed (fetched via
     `https://web.archive.org/web/2023/<original URL>` -- note this
     worked even though `https://archive.org/wayback/available?url=...`
     reported no snapshot for the same URL) that matched `rtmpdump`'s own
     pinned `PKG_SHA256` exactly; staged the same way as the GNU packages
     above.
   Per CoreELEC's own wiki, the result lands under `target/addons/`; this
   is now confirmed against a real run -- exact path is
   `target/addons/<DEVICE>/<KODI_MAJOR_VERSION>/<ARCH>/pvr.dispatcharrai/`,
   e.g. `target/addons/Amlogic-ng/21.3/arm/pvr.dispatcharrai/pvr.dispatcharrai-0.3.0.1.zip`
   for this build (the `.1` is CoreELEC's own `PKG_REV`, not part of this
   addon's own version).
5. Install the resulting zip on the N2+ via Kodi's "install from zip file"
   option, or copy it over SSH and extract into
   `/storage/.kodi/addons/pvr.dispatcharrai/` directly, then restart Kodi.

For later releases, only steps 1-2 and the `cp`/build in steps 3-4 need
repeating -- the CoreELEC checkout itself can be reused, and with the
toolchain and every dependency already built and cached, a rebuild that
only touches this addon's own package is fast (~1 minute wall clock,
confirmed live, versus well over an hour for the first build from
scratch).

### Manual cross-compile + side-load (fallback)

If you'd rather not maintain a package.mk (e.g. testing an unreleased
commit), cross-compile using the same toolchain CoreELEC's build system
uses for the N2+ (`Amlogic-ce`/`Amlogic-ng`, `arm` -- see the confirmed
details above; not `Amlogic-no`/`aarch64`, which was true of an older,
no-longer-recommended branch), matching their exact GCC version and the
Kodi commit their release is built from, then copy the
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
