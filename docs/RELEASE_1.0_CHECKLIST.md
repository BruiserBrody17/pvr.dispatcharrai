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

- **Open question**: what does "tested" mean for 1.0 -- a defined smoke-test
  pass (live playback, recording start/stop, catch-up, timeshift seek, EPG,
  recurring timers) run once per platform right before release, or something
  lighter/heavier?

| Platform | Status |
|---|---|
| Windows | Extensively exercised this session (recordings, catch-up, timeshift, DVR settings, recurring timers) |
| Rocky Linux | Built and deployed; not freshly exercised end-to-end in this session |
| CoreELEC / ODROID | Built and deployed via the beta.1-3 releases; not freshly exercised end-to-end in this session |
| macOS | Only tested in a separate companion session (see `docs/CATCHUP.md`'s macOS `open_mode` investigation) -- not verified against the current build |

## Release packaging

- [x] Kodi addon zips for all 4 platforms -- already automated
      (`.github/workflows/build.yml`), attached to GitHub Releases on every
      version tag. Confirmed live: `1.0.0-beta.3` has all four
      (`*-linux.zip`, `*-osx-arm64.zip`, `*-windows-x86_64.zip`,
      `*-coreelec-arm.zip`) as real downloadable assets.
- [ ] Zip packaging for `dispatcharr-plugin/timeshift_buffer` -- confirmed
      **not** currently packaged at all; its own README tells users to
      manually copy the directory
- [ ] Zip packaging for `dispatcharr-plugin/recording_edl` -- same gap
- [ ] Decide: attach plugin zips to the same GitHub Release as the addon
      itself, or track them separately?

## Open items (more will likely come up)

-
