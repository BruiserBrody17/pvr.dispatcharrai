#!/usr/bin/env python3
"""Flags dangling citations in docs/*.md and CHANGELOG.md against this
repo's own actual citation style: plain-English "docs/X.md's '...'
section" references and backtick-quoted function/setting names -- not
markdown [text](url) links, which a generic link-checker already covers
and this project's docs barely use for cross-references.

Three checks, each independent and best-effort (static text matching, not
a real parser -- expect to eyeball the output, not treat every hit as a
guaranteed bug):

1. Section-title citations ("docs/X.md's "Some Heading" section") against
   that file's real markdown headings.
2. Backtick-quoted function calls ("`SomeFunction()`") against src/*.cpp
   and src/*.h.
3. Backtick-quoted setting ids, only on lines that also mention the word
   "setting", against resources/settings.xml's real <setting id="..."> list.

Baselined against tools/doc_refs_baseline.txt: a first run against docs
that had never been checked before flagged 76 hits, almost all legitimate
-- external (Kodi-core/library/stdlib) citations this project's docs
deliberately make, or citations to a reverted approach's now-removed code
(docs/*.md's whole job includes preserving that history, per CLAUDE.md).
Re-triaging that same debt on every run isn't useful, so the baseline
records today's known findings (keyed on the citation itself, not its
line number, so unrelated doc edits don't shift it) and only a genuinely
NEW dangling reference -- not in the baseline -- fails the run.

Usage:
  python tools/check_doc_refs.py                  # fail only on new findings
  python tools/check_doc_refs.py --update-baseline # accept current findings as the new baseline
Exit code 0 if nothing new is flagged, 1 otherwise.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DOCS_DIR = REPO_ROOT / "docs"
DOC_FILES = sorted(DOCS_DIR.glob("*.md")) + [REPO_ROOT / "CHANGELOG.md"]
SRC_DIR = REPO_ROOT / "src"
PLUGIN_FILES = sorted((REPO_ROOT / "dispatcharr-plugin").glob("*/plugin.py"))
SETTINGS_XML = REPO_ROOT / "pvr.dispatcharrai" / "resources" / "settings.xml"
BASELINE_PATH = Path(__file__).resolve().parent / "doc_refs_baseline.txt"

SECTION_TITLE_RE = re.compile(r'"([^"]{4,100})"\s+section')
# Either a bare "docs/X.md" mention or a relative markdown link whose
# target is a local .md file -- "[TIMESHIFT.md](TIMESHIFT.md)" is a real,
# common citation form within docs/ itself (no "docs/" prefix needed
# there), not just "docs/X.md" written out in prose.
DOC_FILE_REF_RE = re.compile(r"docs/(\w+\.md)|\]\((\w+\.md)\)")
FUNCTION_REF_RE = re.compile(r"`([A-Za-z_][A-Za-z0-9_:]*)\(\)`")
SETTING_REF_RE = re.compile(r"`([a-z][a-z0-9_]*_[a-z0-9_]*)`")

# Placeholder/self-referential text this project's own docs use when
# pointing at "whatever section was already under discussion" rather than
# naming a real heading -- not a real citation to check.
GENERIC_TITLES = {"same", "...", "Section Title"}

HEADING_RE = re.compile(r"^#{1,6}\s+(.*)")
# This project's docs use a bold-leading paragraph/bullet as a pseudo-
# heading at least as often as a real "#" heading, e.g. "- **The
# permission requirement**: ..." or "**Update (2026-09-08):** ..." --
# both are legitimate citation targets for a "...' section" reference.
BOLD_PSEUDO_HEADING_RE = re.compile(r"^[\s\-*]*\*\*(.+?)\*\*:?")


def normalize(text: str) -> str:
    return re.sub(r"[`*_]", "", text).strip().lower()


def get_headings(path: Path) -> set[str]:
    headings = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        m = HEADING_RE.match(line) or BOLD_PSEUDO_HEADING_RE.match(line)
        if m:
            headings.add(normalize(m.group(1)))
    return headings


def check_section_titles() -> list[tuple[str, str]]:
    errors = []
    heading_cache: dict[Path, set[str]] = {}
    for doc in DOC_FILES:
        # Which docs/X.md a "..." section citation belongs to is often
        # stated a line or two earlier, since this project's prose wraps
        # long sentences across lines -- remember the most recent mention
        # within the current paragraph (reset at blank lines) rather than
        # only checking the exact line the quote falls on.
        last_doc_file: str | None = None
        for lineno, line in enumerate(doc.read_text(encoding="utf-8").splitlines(), start=1):
            if not line.strip():
                last_doc_file = None
                continue
            file_match = DOC_FILE_REF_RE.search(line)
            if file_match:
                last_doc_file = file_match.group(1) or file_match.group(2)
            for m in SECTION_TITLE_RE.finditer(line):
                title = m.group(1).rstrip(".")
                if title in GENERIC_TITLES:
                    continue
                target_name = (file_match.group(1) or file_match.group(2)) if file_match else last_doc_file
                target_path = DOCS_DIR / target_name if target_name else doc
                if not target_path.exists():
                    continue
                if target_path not in heading_cache:
                    heading_cache[target_path] = get_headings(target_path)
                headings = heading_cache[target_path]
                normalized_title = normalize(title)
                if not any(normalized_title in h or h in normalized_title for h in headings):
                    doc_rel = doc.relative_to(REPO_ROOT)
                    target_rel = target_path.relative_to(REPO_ROOT)
                    key = f"{doc_rel}|section|{title}|{target_rel}"
                    message = f'{doc_rel}:{lineno}: cites "{title}" section in {target_rel}, no matching heading found'
                    errors.append((key, message))
    return errors


def check_functions() -> list[tuple[str, str]]:
    errors = []
    code_files = list(SRC_DIR.glob("*.cpp")) + list(SRC_DIR.glob("*.h")) + PLUGIN_FILES
    src_text = "\n".join(p.read_text(encoding="utf-8") for p in code_files)
    for doc in DOC_FILES:
        doc_rel = doc.relative_to(REPO_ROOT)
        for lineno, line in enumerate(doc.read_text(encoding="utf-8").splitlines(), start=1):
            for m in FUNCTION_REF_RE.finditer(line):
                name = m.group(1).split("::")[-1]
                if not re.search(rf"\b{re.escape(name)}\b", src_text):
                    key = f"{doc_rel}|function|{name}"
                    message = (
                        f"{doc_rel}:{lineno}: references `{name}()`, not found in src/ or either "
                        "plugin's plugin.py -- verify it isn't an external (Kodi-core/library/stdlib) "
                        "citation before treating as stale"
                    )
                    errors.append((key, message))
    return errors


def get_setting_ids() -> set[str]:
    # The Kodi addon's own settings.xml, plus each plugin's own
    # Dispatcharr-side setting/action ids (a separate namespace, defined
    # in plugin.py's own settings schema, not settings.xml at all).
    known = set(re.findall(r'<setting id="([^"]+)"', SETTINGS_XML.read_text(encoding="utf-8")))
    for plugin_file in PLUGIN_FILES:
        known |= set(re.findall(r'"id":\s*"([a-z][a-z0-9_]*)"', plugin_file.read_text(encoding="utf-8")))
    return known


def check_settings() -> list[tuple[str, str]]:
    errors = []
    known = get_setting_ids()
    for doc in DOC_FILES:
        doc_rel = doc.relative_to(REPO_ROOT)
        for lineno, line in enumerate(doc.read_text(encoding="utf-8").splitlines(), start=1):
            if "setting" not in line.lower():
                continue
            for m in SETTING_REF_RE.finditer(line):
                token = m.group(1)
                if token not in known:
                    key = f"{doc_rel}|setting|{token}"
                    message = (
                        f"{doc_rel}:{lineno}: references setting `{token}`, not found in "
                        f"{SETTINGS_XML.relative_to(REPO_ROOT)} or either plugin's own settings/actions -- "
                        "verify it isn't a Dispatcharr API field or a deliberately-documented removed setting"
                    )
                    errors.append((key, message))
    return errors


def load_baseline() -> set[str]:
    if not BASELINE_PATH.exists():
        return set()
    return {line.strip() for line in BASELINE_PATH.read_text(encoding="utf-8").splitlines() if line.strip()}


def write_baseline(keys: set[str]) -> None:
    BASELINE_PATH.write_text("\n".join(sorted(keys)) + "\n", encoding="utf-8")


def main() -> int:
    findings = check_section_titles() + check_functions() + check_settings()

    if "--update-baseline" in sys.argv[1:]:
        write_baseline({key for key, _ in findings})
        print(f"check_doc_refs: wrote {len(findings)} finding(s) to {BASELINE_PATH.relative_to(REPO_ROOT)}")
        return 0

    baseline = load_baseline()
    new_findings = [message for key, message in findings if key not in baseline]
    resolved_count = len(baseline - {key for key, _ in findings})

    if not new_findings:
        print(f"check_doc_refs: no new dangling references ({len(findings)} known, baselined)")
        if resolved_count:
            print(
                f"check_doc_refs: {resolved_count} baseline entr{'y is' if resolved_count == 1 else 'ies are'} "
                "no longer triggering -- consider running --update-baseline to prune them"
            )
        return 0

    print(f"check_doc_refs: {len(new_findings)} new possibly-dangling reference(s), not in the baseline:\n")
    for message in new_findings:
        print(message)
    print(
        "\nIf these are legitimate (an external citation, or documenting a reverted approach), "
        "run with --update-baseline to accept them."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
