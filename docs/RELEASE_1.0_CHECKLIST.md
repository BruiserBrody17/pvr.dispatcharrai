*(tracked punch-list for the 1.0 release -- not part of the "confirmed live" API_NOTES.md family of docs; check items off and add new ones here as they come up)*

# 1.0 release checklist

## Documentation

- [x] README cleaned up and made concise -- rewritten from 211 to ~90
      lines: install/config/features only, narrative moved out. Decided
      **not** to physically relocate the existing `docs/*.md` files --
      ~40+ source-code comments (C++ and the Dispatcharr plugins' own
      Python) point at their exact current paths, so moving them would be
      a large, error-prone churn for no real benefit. Instead added a
      one-line disclaimer at the top of `docs/API_NOTES.md` marking that
      whole family as engineering history, not user docs, with a pointer
      back to the main README.
- [x] `dispatcharr-plugin/timeshift_buffer/README.md` (186 -> 70 lines)
      and `dispatcharr-plugin/recording_edl/README.md` (226 -> 61 lines)
      rewritten. Their narrative wasn't already covered elsewhere the way
      the main addon's was, so first backfilled the missing history into
      `docs/TIMESHIFT.md` (already had it -- just pointed there) and
      `docs/RECORDING_EDL.md` (didn't yet cover the orphan-sidecar-cleanup
      or `.dvr_*_hls` diagnostics/delete features added this session --
      added those sections), then trimmed each plugin README to
      install/actions/safety-essentials with a pointer back for the "why."
      Kept the safety-critical facts in place (what each destructive
      action does and doesn't touch, the `.dvr_*_hls` classification
      table) rather than treating them as narrative to cut.

## Platform testing

Four platforms: Windows, Rocky Linux, CoreELEC/ODROID, macOS.

**Smoke-test pass, defined**: live playback, recording start/stop,
catch-up, timeshift seek, EPG, recurring timers -- run once per platform
right before release.

| Platform | Status |
|---|---|
| Windows | Recording start/stop, catch-up, recurring timers, EPG, and live playback (`Off`, `Local`, and `Server-side` modes) all verified this session against a fresh build. Live-mode check also confirmed a real, practical thing: an existing profile's persisted `live_timeshift_mode` value survives the code-level default change unchanged (`default="true"` in settings.xml does **not** get re-resolved to the addon's new default on load) -- server-side kept working exactly as before for this account, Off was separately confirmed clean (`STREAMURL` set, `canseek: false` as designed, stable playback, zero errors), and `Local` (reintroduced this session -- see `docs/TIMESHIFT.md`) was confirmed with an actual seek: backward and forward both landed correctly (`demuxer seek to: ..., success`, ffmpegdirect's own `TimeshiftBuffer::Seek` locating the right segment/packet) and playback resumed cleanly both times. Timeshift *seek* itself under **server-side** mode specifically (as opposed to Local, or the in-progress-recording variant, both now verified) is the one remaining gap -- the underlying server-side seek code didn't change this session, so this is a documentation gap, not a suspected regression. |
| Rocky Linux | Built and deployed; not freshly exercised end-to-end in this session |
| CoreELEC / ODROID | Built and deployed via the beta.1-3 releases; not freshly exercised end-to-end in this session |
| macOS | Only tested in a separate companion session (see `docs/CATCHUP.md`'s macOS `open_mode` investigation) -- not verified against the current build |

## Release packaging

- [x] Kodi addon zips for all 4 platforms -- already automated
      (`.github/workflows/build.yml`), attached to GitHub Releases on every
      version tag. Confirmed live: `1.0.0-beta.3` has all four
      (`*-linux.zip`, `*-osx-arm64.zip`, `*-windows-x86_64.zip`,
      `*-coreelec-arm.zip`) as real downloadable assets.
- [x] Zip packaging for `dispatcharr-plugin/timeshift_buffer` and
      `dispatcharr-plugin/recording_edl` -- new `package-dispatcharr-plugins`
      job in `.github/workflows/build.yml`, attached to the same GitHub
      Release as the addon zips on the same version tag. Confirmed live via
      `gh run watch` (all steps green) and by downloading the actual CI
      artifact and inspecting it with `unzip -l`: each zip's top-level entry
      is exactly the plugin's own directory name
      (`timeshift_buffer/`/`recording_edl/`) containing only
      `plugin.py`/`plugin.json`/`README.md` -- no stray `__pycache__`, no
      extra nesting.

## Open items (more will likely come up)

- Local timeshift mode (`live_timeshift_mode = 1`) was reintroduced this
  session -- verified live on Windows only. The other three platforms'
  smoke-test passes should include it alongside server-side/Off, since it
  depends on a separate addon (`inputstream.ffmpegdirect`) whose
  availability/behavior could plausibly differ by platform in a way
  nothing else in this checklist would catch.
