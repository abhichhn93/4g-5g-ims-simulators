#!/usr/bin/env python3
# quiz.py — Offline CLI quiz tool for the telecom simulator interview question bank.
# Loads all YAML files from interview_questions/, filters by domain and level,
# presents questions interactively. No simulator needs to be running.
# Usage: python3 tools/quiz.py [--domain cpp|4g|5g|ims] [--level engineer|senior] [--count 5]

import argparse
import glob
import pathlib
import random
import sys
import time

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml not installed — run: pip install pyyaml")
    sys.exit(1)

REPO_ROOT      = pathlib.Path(__file__).parent.parent.resolve()
QUESTIONS_GLOB = str(REPO_ROOT / "interview_questions" / "*.yaml")
FALLBACK_FILE  = REPO_ROOT / "interview_questions.yaml"

# ── ANSI colors ──────────────────────────────────────────────────────────────
R  = '\033[0m'
B  = '\033[1m'       # bold
CY = '\033[1;36m'    # cyan
GR = '\033[1;32m'    # green
YE = '\033[1;33m'    # yellow
RD = '\033[1;31m'    # red
BL = '\033[1;34m'    # blue
MA = '\033[1;35m'    # magenta

def print_banner():
    print(f"""
{BL}╔══════════════════════════════════════════════════════════╗
║  4G/5G/IMS Telecom + C++ Interview Practice Quiz        ║
║  Press ENTER to reveal answer, 'q' + ENTER to quit      ║
╚══════════════════════════════════════════════════════════╝{R}""")

# ── Load questions ─────────────────────────────────────────────────────────────
def load_questions():
    qs = []
    paths = glob.glob(QUESTIONS_GLOB)
    if paths:
        for path in paths:
            try:
                with open(path) as f:
                    data = yaml.safe_load(f)
                if isinstance(data, list):
                    qs.extend(data)
                elif isinstance(data, dict) and "questions" in data:
                    qs.extend(data["questions"])
            except Exception as e:
                print(f"[quiz] WARNING: could not parse {path}: {e}", file=sys.stderr)
    elif FALLBACK_FILE.exists():
        with open(FALLBACK_FILE) as f:
            data = yaml.safe_load(f)
        if isinstance(data, list):
            qs = data
        elif isinstance(data, dict) and "questions" in data:
            qs = data["questions"]
    return qs

# ── Filtering ──────────────────────────────────────────────────────────────────
def filter_questions(qs, domain=None, level=None):
    result = []
    for q in qs:
        if not isinstance(q, dict):
            continue
        q_domain  = str(q.get("domain", "")).lower()
        q_level   = str(q.get("level",  "")).lower()
        q_tags    = [str(t).lower() for t in q.get("tags", [])]
        q_text    = str(q.get("question", q.get("q", ""))).strip()
        q_answer  = str(q.get("answer",   q.get("a", ""))).strip()
        if not q_text or not q_answer:
            continue
        if domain:
            dl = domain.lower()
            if dl not in q_domain and dl not in q_tags and not any(dl in t for t in q_tags):
                continue
        if level:
            ll = level.lower()
            if ll and ll not in q_level:
                continue
        result.append(q)
    return result

# ── Quiz runner ───────────────────────────────────────────────────────────────
def run_quiz(questions, count):
    random.shuffle(questions)
    selected = questions[:count]
    total    = len(selected)
    if total == 0:
        print(f"{RD}No questions matched your filters.{R}")
        return

    print(f"\n{GR}Found {total} questions. Starting quiz…{R}\n")
    time.sleep(0.5)

    score = 0
    for i, q in enumerate(selected, 1):
        q_text   = q.get("question", q.get("q", "")).strip()
        q_answer = q.get("answer",   q.get("a", "")).strip()
        domain   = q.get("domain", "").upper()
        level    = q.get("level", "engineer").upper()
        tags     = q.get("tags", [])

        # ── Question ──
        print(f"{B}{'─'*60}{R}")
        print(f"{CY}[{i}/{total}] [{domain}] [{level}]{R}")
        print(f"\n{B}{q_text}{R}\n")

        answer = input(f"  Press ENTER to see answer (or 'q' to quit)… ").strip().lower()
        if answer == 'q':
            print(f"\n{YE}Quiz aborted at question {i}/{total}.{R}")
            break

        # ── Answer ──
        print(f"\n{GR}Answer:{R}")
        # Word-wrap the answer at ~70 chars
        for line in q_answer.split('\n'):
            words = line.split()
            cur   = "  "
            for w in words:
                if len(cur) + len(w) > 72:
                    print(cur); cur = "  " + w + " "
                else:
                    cur += w + " "
            if cur.strip():
                print(cur)

        if tags:
            tag_str = "  ".join(f"[{t}]" for t in tags[:5])
            print(f"\n  {MA}tags: {tag_str}{R}")
        print()

        # Self-grading
        grade = input(f"  Did you get it? ({GR}y{R}/{RD}n{R}) ").strip().lower()
        if grade == 'y':
            score += 1
            print(f"  {GR}+1 ✓{R}\n")
        else:
            print(f"  {RD}Keep practicing!{R}\n")

    # ── Summary ──
    pct = (score / total * 100) if total else 0
    print(f"\n{B}{'═'*60}{R}")
    print(f"{B}Quiz complete!  Score: {GR}{score}{R}{B}/{total} ({pct:.0f}%){R}")
    if pct >= 80:
        print(f"  {GR}Excellent — you're ready for a senior interview!{R}")
    elif pct >= 50:
        print(f"  {YE}Good progress — review the missed questions.{R}")
    else:
        print(f"  {RD}Keep studying — run again with fewer questions to build confidence.{R}")
    print()

# ── CLI entry point ───────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Offline interview quiz from the telecom simulator question bank",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 tools/quiz.py                          # random 5 from all domains
  python3 tools/quiz.py --domain cpp             # C++ patterns only
  python3 tools/quiz.py --domain 4g --count 10  # 10 4G protocol questions
  python3 tools/quiz.py --domain ims --level senior
  python3 tools/quiz.py --domain 5g --level engineer
""")
    parser.add_argument("--domain", choices=["cpp", "4g", "5g", "ims", "all"],
                        default=None, help="Filter by domain (default: all)")
    parser.add_argument("--level",  choices=["engineer", "senior", "all"],
                        default=None, help="Filter by seniority level (default: all)")
    parser.add_argument("--count",  type=int, default=5,
                        help="Number of questions (default: 5)")
    parser.add_argument("--list-domains", action="store_true",
                        help="Show available domains and question counts, then exit")
    args = parser.parse_args()

    qs = load_questions()
    if not qs:
        print(f"{RD}ERROR: No questions found. Check interview_questions/ folder.{R}")
        sys.exit(1)

    if args.list_domains:
        from collections import Counter
        domains = Counter(str(q.get("domain","?")).lower() for q in qs if isinstance(q, dict))
        print(f"\n{BL}Available domains:{R}")
        for d, cnt in sorted(domains.items(), key=lambda x: -x[1]):
            print(f"  {B}{d:12s}{R}  {cnt} questions")
        print(f"\n  Total: {len(qs)} questions\n")
        return

    domain = None if args.domain == "all" else args.domain
    level  = None if args.level  == "all" else args.level
    filtered = filter_questions(qs, domain, level)

    print_banner()
    if domain or level:
        info = []
        if domain: info.append(f"domain={domain.upper()}")
        if level:  info.append(f"level={level.upper()}")
        print(f"  {CY}Filter: {', '.join(info)}{R}")
        print(f"  {CY}Matched: {len(filtered)} questions (showing {min(args.count, len(filtered))}){R}\n")

    run_quiz(filtered, args.count)

if __name__ == "__main__":
    main()
