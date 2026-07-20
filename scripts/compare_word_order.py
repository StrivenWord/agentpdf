#!/usr/bin/env python3
"""Compare agentpdf Markdown output to handmade goldens (normalized word order)."""

from __future__ import annotations

import re
import sys
from pathlib import Path


def strip_yaml(text: str) -> str:
    if text.startswith("---"):
        parts = text.split("---", 2)
        if len(parts) >= 3:
            return parts[2]
    return text


def normalize_handmade_artifacts(text: str) -> str:
    # Known handmade typos / wrap artifacts that are not PDF truth.
        "beable": "be able",
        "tobeable": "to be able",
        "ofits": "of its",
        "formost": "for most",
        "sources formost": "sources for most",
        "content ofan": "content of an",
        "thefidelity": "the fidelity",
        "sum ofits": "sum of its",
        "able towork": "able to work",
        "exist- ing": "existing",
        "con- tribute": "contribute",
        "van- ished": "vanished",
        "to- day": "today",
        "indefinately": "indefinitely",
        "wordswith": "words with",
        "andadding": "and adding",
        "publicare": "public are",
        "tolibrarians": "to librarians",
        "inthe": "in the",
        "sum ofits parts": "sum of its parts",
    }
    low = text
    for a, b in reps.items():
        low = re.sub(re.escape(a), b, low, flags=re.I)
    return low


def prepare_candidate_gold(text: str) -> str:
    m = re.search(r"(?im)^What explains the explosion", text)
    if m:
        text = text[m.start() :]
    text = re.sub(r"(?im)^# African coups.*\n", "", text)
    text = re.sub(
        r"(?is)\nChin JJ and Kirkpatrick.*?(?=\n## 1\.|\n# 1\.)",
        "\n",
        text,
    )
    return text


def words(text: str) -> list[str]:
    text = strip_yaml(text)
    text = normalize_handmade_artifacts(text)
    text = text.lower()
    text = (
        text.replace("\u2019", "'")
        .replace("\u2018", "'")
        .replace("\u201c", '"')
        .replace("\u201d", '"')
        .replace("ﬁ", "fi")
        .replace("ﬂ", "fl")
    )
    text = re.sub(r"([a-z0-9])-\s*([a-z0-9])", r"\1\2", text)
    return re.findall(r"[a-z0-9']+", text)


def compare(out_path: Path, gold_path: Path) -> int:
    out_text = out_path.read_text(encoding="utf-8")
    gold_text = gold_path.read_text(encoding="utf-8")
    if "candidate_paper" in gold_path.name:
        gold_text = prepare_candidate_gold(strip_yaml(gold_text))
        # rebuild with yaml already stripped
        gold_words = words("---\n---\n" + gold_text)
    else:
        gold_words = words(gold_text)
    out_words = words(out_text)
    n = min(len(out_words), len(gold_words))
    mis = next((i for i in range(n) if out_words[i] != gold_words[i]), None)
    matched = n if mis is None else mis
    pct = 100.0 * matched / max(len(gold_words), 1)
    print(
        f"{out_path.name}: out={len(out_words)} gold={len(gold_words)} "
        f"mismatch={mis} matched={matched} ({pct:.1f}%)"
    )
    if mis is not None:
        a = max(0, mis - 5)
        b = mis + 12
        print("  out :", " ".join(out_words[a:b]))
        print("  gold:", " ".join(gold_words[a:b]))
        return 1
    if len(out_words) != len(gold_words):
        print(f"  prefix ok; length delta={len(out_words) - len(gold_words)}")
        return 1
    print("  PERFECT")
    return 0


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build" / "test-out"
    gold_dir = (
        Path(sys.argv[2])
        if len(sys.argv) > 2
        else root.parent / "unit-testing" / "md-rendering" / "handmade-extractions"
    )
    pairs = [
        ("acm_crane2001.md", "acm_crane2001_handmade.md"),
        ("acm_bernstein2002.md", "acm_bernstein2002_handmade.md"),
        ("candidate_paper.md", "candidate_paper_handmade.md"),
    ]
    rc = 0
    for o, g in pairs:
        rc |= compare(out_dir / o, gold_dir / g)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
