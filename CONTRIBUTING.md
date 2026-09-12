# Contributing to pvr.dispatcharr-unofficial

Thanks for looking at this. A few things worth knowing before you open
a PR -- this project is pre-1.0, single-maintainer, and has no
automated test suite, all of which shape how contributions get handled
here.

## Before you start

- **There's no automated test suite.** Verification is manual:
  smoke-testing against a real Dispatcharr instance and a real (or
  emulated) Kodi install. If your change touches the C++ addon or
  either Python plugin's actual behavior, you'll need a way to test it
  live -- a Dispatcharr instance you control, plus Kodi on at least one
  platform. If you can't test a change end-to-end, say so plainly in
  the PR rather than asserting it works.
- **The addon can't be built standalone.** It builds through Kodi's own
  binary-addon build harness. Full, previously-verified build steps for
  Windows/macOS/Linux/CoreELEC are in [docs/BUILDING.md](docs/BUILDING.md)
  -- start there rather than guessing at commands.
- **CI only covers part of this.** `.github/workflows/build.yml`
  compiles the addon on Windows/macOS/Linux and packages the two
  plugins as zips -- it doesn't build or test the CoreELEC package, and
  doesn't exercise runtime behavior on any platform. Green CI means
  "it compiles and lints," not "it works."

## Where things live

- `src/` -- the addon's C++ source.
- `pvr.dispatcharr-unofficial/` -- addon metadata Kodi actually loads
  (`addon.xml.in`, `resources/settings.xml`, the language file).
- `dispatcharr-plugin/timeshift_buffer/`, `dispatcharr-plugin/recording_edl/`
  -- the two server-side Python plugins, independent of the addon and
  of each other. Each has its own README.
- `docs/` -- engineering history for this project: root causes, things
  confirmed live against a real Dispatcharr instance, approaches tried
  and reverted. Written for people working on the code, not end users
  -- if you're looking for how to *use* the addon, see
  [README.md](README.md) instead.
- `docs/OPEN_ITEMS.md` -- the running punch-list of known gaps and
  in-progress investigations. Worth checking before starting something
  substantial, in case it's already tracked (or already ruled out).

## Writing your change

- **Format before you push.** `clang-format -i src/*.cpp src/*.h` for
  C++, `ruff format dispatcharr-plugin/` for Python. `ruff check
  dispatcharr-plugin/` catches some real bugs too (unused variables,
  etc.), not just style -- run it.
- **Comments explain WHY, not WHAT.** Document non-obvious constraints,
  something you confirmed live, or a workaround for a specific bug --
  not a restatement of what the next line obviously does.
- **If you confirmed something against a real Dispatcharr instance,
  say so specifically** -- a real endpoint response, a real device
  test, the actual result. Dispatcharr's own API has changed shape
  across releases, so "I checked the real behavior" is much more
  useful here than "this should work per the docs."
- **Never include a real hostname, IP address, account username, or
  similar identifying detail** in code, comments, docs, commit
  messages, or the PR description itself -- including your own, and
  including anything from your own test instance's logs or output you
  might paste in as evidence. If you're documenting a real channel or
  programme name from your own setup, genericize it the way this
  project's own docs already do elsewhere (e.g. "Channel A"). This
  isn't a formality: getting this wrong after the fact means a git
  history rewrite, not just an edit.
- **Explain what you changed and why in the PR description**, including
  how you tested it (or that you couldn't, and why). This project's own
  commit history favors a real explanation over a one-line summary --
  a short PR that just says "fixes bug" with no context is much harder
  to review with no test suite backing it up.
- Don't worry about crafting a clean commit history on your own
  branch -- accepted PRs get squash-merged into one commit, so your
  branch's intermediate commits don't end up in `master`'s history
  either way.

## Versioning

The addon and each of the two plugins version independently (see
[CHANGELOG.md](CHANGELOG.md) for the pattern). You don't need to bump a
version yourself as part of a PR -- the maintainer handles that at
merge/release time. If you do want to include one, see `CLAUDE.md`'s
versioning bullets for the exact mechanics (there are a few
easy-to-miss spots per piece).

## What happens after you open a PR

Every external PR gets an actual manual review before merging -- never
auto-merged just because CI is green. With no automated test suite,
passing CI proves the change compiles and formats cleanly, not that
it's correct, so a real read-through matters more here than on a
project with real test coverage. This is a single-maintainer project,
so review may take a while; that's not a signal your PR was rejected.

## Reporting a security issue

Don't open a public issue for a security report -- see
[SECURITY.md](SECURITY.md) for how to report privately instead.

## License

Contributions are made under this project's license,
GPL-2.0-or-later.
