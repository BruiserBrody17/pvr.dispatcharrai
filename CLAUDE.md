# pvr.dispatcharrai

A Kodi PVR binary addon (C++) for [Dispatcharr](https://github.com/Dispatcharr/Dispatcharr),
plus two optional Python plugins that install on the Dispatcharr server
itself. See [README.md](README.md) for what it does; this file is for
working in the codebase, not using the addon.

## Repo layout

- `src/` -- the addon's C++ source (`DispatcharrClient` talks to
  Dispatcharr's REST/JSON-RPC API, `PVRDispatcharr` implements Kodi's PVR
  API surface, `XmlTvParser`/`WebSocketClient` are self-contained helpers).
- `pvr.dispatcharrai/` -- addon metadata Kodi actually loads: `addon.xml.in`
  (version lives here), `resources/settings.xml`,
  `resources/language/resource.language.en_gb/strings.po`.
- `dispatcharr-plugin/timeshift_buffer/`, `dispatcharr-plugin/recording_edl/`
  -- independent Python plugins for the Dispatcharr server, not built or
  installed through Kodi at all. Each has its own README and `plugin.json`.
- `packaging/coreelec/` -- out-of-tree CoreELEC package definition (not
  used by the GitHub Actions build).
- `docs/` -- engineering history: root causes, live-confirmed API
  behavior, things tried and reverted. Not user-facing.
- `docs/RELEASE_1.0_CHECKLIST.md` -- the running punch-list for release
  work; add new open items there rather than losing track of them in
  conversation.

## Building and testing

This addon cannot be compiled standalone -- it builds through Kodi's own
binary-addon build harness, which needs a matching Kodi source checkout.
Full instructions (Windows/macOS/Linux/CoreELEC, all previously verified
live) are in [docs/BUILDING.md](docs/BUILDING.md); don't guess at build
commands, read that file.

There is no automated test suite. Verification is manual: smoke-testing
against a real Dispatcharr instance and real/emulated Kodi installs
(Windows, macOS, Linux via Kodi Flatpak, CoreELEC on an ODROID N2+), driven
via Kodi's JSON-RPC webserver. `.github/workflows/build.yml` only compiles
Windows/macOS/Linux and packages the two plugins -- it does not build or
test the CoreELEC package, and doesn't exercise runtime behavior on any
platform.

## Conventions specific to this repo

- **Docs split by audience**: `README.md` and each plugin's own `README.md`
  are concise and user-facing -- install/configure/use only. `docs/*.md`
  holds the "why" (investigations, root causes, API behavior confirmed
  against a live Dispatcharr instance, reverted approaches) and is never
  meant to be read by an end user. `CHANGELOG.md` is user-facing
  what-changed, not why. When you learn something new about Dispatcharr's
  API or fix a non-obvious bug, the explanation belongs in `docs/`, not
  buried in a commit message.
- **"Confirmed live" citations matter**: Dispatcharr is young and its API
  schema has changed across releases. Comments and docs here frequently
  cite exactly how something was confirmed (a real endpoint response, a
  real device test) rather than just asserting behavior -- keep doing
  that instead of trusting Dispatcharr's own docs/schema at face value.
- **Comments explain WHY, not WHAT**: this codebase's existing comments
  document non-obvious constraints, confirmed-live findings, and
  workarounds for specific bugs -- not a restatement of the code. Match
  that style; don't add narrative comments describing what a block of
  code obviously does.
- **Three independent version numbers, decoupled since 1.0.1** -- the
  addon and each of the two companion plugins version separately. Bump
  only the piece whose own files actually changed in a given release;
  don't bump a plugin just because the addon released, or vice versa
  (pre-1.0 releases moved all three together on purpose, to signal "the
  1.0-era plugins" -- that was a deliberate one-time exception, not the
  ongoing policy). Whichever piece(s) you *are* bumping, all of that
  piece's own version locations still need to move together -- it's easy
  to miss one:
  - Addon: `pvr.dispatcharrai/addon.xml.in` (`<addon version="...">`) and
    `packaging/coreelec/pvr.dispatcharrai/package.mk` (`PKG_VERSION`,
    with `PKG_SHA256` reset to the all-zeros placeholder until the tag
    exists and the real checksum can be computed) move together -- CoreELEC
    packages the addon binary, not either plugin, so this pair only moves
    when the addon's version does, regardless of what the plugins are doing.
  - Each plugin's `plugin.json` **and** its own `plugin.py`'s `Plugin`
    class `version` attribute move together -- `plugin.json` is only used
    for Dispatcharr's not-yet-trusted import preview; the hardcoded class
    attribute in `plugin.py` is what Dispatcharr actually runs once
    trusted. Bumping only `plugin.json` is a real, easy-to-miss mistake.
  A git tag/GitHub Release is still addon-version-scoped (that's what
  triggers CI); both plugins' zips get attached to it regardless of
  whether their own version moved, since `package-dispatcharr-plugins`
  zips whatever's currently committed either way. `CHANGELOG.md` should
  say explicitly which piece(s) moved in a given entry rather than
  implying all three share one number.
- **The CoreELEC package isn't part of CI** and won't be (see
  `docs/BUILDING.md`'s "GitHub Actions job ... rejected" note --
  CoreELEC's build harness assumes persistent, self-hosted infrastructure
  and a large pre-built toolchain cache, not a one-shot cloud runner). A
  real tagged release needs its CoreELEC zip built and attached to the
  GitHub Release by hand; the exact steps, including the `gh release
  upload` command, are documented at the end of that file's CoreELEC
  section.
- **Commit messages**: short imperative summary line, no conventional-
  commit type prefixes (no `fix:`/`feat:`) -- match the existing git log.

## License

GPL-2.0-or-later.
