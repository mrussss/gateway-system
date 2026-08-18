#!/usr/bin/env python3
"""Check that repository-local Markdown links resolve to files and headings."""

from __future__ import annotations

import re
import subprocess
import sys
import urllib.parse
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
LINK = re.compile(r"(?<!!)\[[^\]]*\]\((<[^>]+>|[^)\s]+)(?:\s+['\"(].*?[)'\"])?\)")
HEADING = re.compile(r"^#{1,6}\s+(.+?)\s*$", re.MULTILINE)


def slug(text: str) -> str:
    value = re.sub(r"<[^>]+>", "", text).strip().lower()
    value = re.sub(r"[^\w\-\s\u4e00-\u9fff]", "", value)
    return re.sub(r"[\s-]+", "-", value).strip("-")


def tracked_markdown() -> list[Path]:
    output = subprocess.check_output(
        ["git", "-C", str(ROOT), "ls-files", "*.md"], text=True
    )
    return [ROOT / line for line in output.splitlines() if line]


def main() -> int:
    failures: list[str] = []
    checked = 0
    for source in tracked_markdown():
        content = source.read_text(encoding="utf-8")
        for match in LINK.finditer(content):
            raw = match.group(1).strip("<>")
            if raw.startswith(("http://", "https://", "mailto:", "tel:")):
                continue
            parsed = urllib.parse.urlsplit(raw)
            target_path = urllib.parse.unquote(parsed.path)
            target = source if not target_path else (source.parent / target_path).resolve()
            checked += 1
            if not target.exists():
                failures.append(f"{source.relative_to(ROOT)}: missing {raw}")
                continue
            if parsed.fragment and target.is_file() and target.suffix.lower() == ".md":
                headings = {slug(value) for value in HEADING.findall(target.read_text(encoding="utf-8"))}
                if urllib.parse.unquote(parsed.fragment).lower() not in headings:
                    failures.append(
                        f"{source.relative_to(ROOT)}: missing heading #{parsed.fragment} in "
                        f"{target.relative_to(ROOT)}"
                    )
    if failures:
        print("\n".join(f"[docs-link] FAIL {failure}" for failure in failures), file=sys.stderr)
        return 1
    print(f"[docs-link] PASS {checked} local links across {len(tracked_markdown())} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
