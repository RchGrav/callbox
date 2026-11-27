#!/usr/bin/env bash
# The 500-line rule (AGENTS.md, "File size"): no first-party source, header,
# script or test file over 500 lines. Why: a file a person or an agent can read
# whole is one they can change safely; past that, a file holds more than one
# responsibility and its parts hide each other.
#
# Files over the limit when the rule began are listed in
# scripts/lint_lines.allow with their size then as a ceiling: they may shrink,
# never grow, and come off the list once split. Generated files (marked "Do not
# edit") and vendored code (third_party/) are not first-party and are skipped.
set -euo pipefail
cd "$(dirname "$0")/.."
LIMIT=500
allow=scripts/lint_lines.allow
fail=0
while IFS= read -r f; do
  head -3 "$f" | grep -q 'Do not edit' && continue
  n=$(wc -l <"$f")
  ceiling=$(awk -v f="$f" '$2 == f { print $1 }' "$allow" 2>/dev/null || true)
  if [ -n "$ceiling" ]; then
    if [ "$n" -gt "$ceiling" ]; then
      echo "lint_lines: $f has $n lines; it was over $LIMIT at $ceiling and may only shrink" >&2
      fail=1
    fi
  elif [ "$n" -gt "$LIMIT" ]; then
    echo "lint_lines: $f has $n lines; the limit is $LIMIT (AGENTS.md, File size)" >&2
    fail=1
  fi
done < <(git ls-files --cached --others --exclude-standard -- 'src/*' 'include/*' 'scripts/*' 'tests/*' 'Makefile' |
         grep -E '\.(c|h|sh|py)$|^Makefile$' | while IFS= read -r p; do [ -f "$p" ] && echo "$p"; done)
# a listed file that is now within the limit should leave the list
while read -r ceiling f; do
  [ -z "${f:-}" ] && continue
  case "$ceiling" in \#*) continue ;; esac
  if [ -f "$f" ] && [ "$(wc -l <"$f")" -le "$LIMIT" ]; then
    echo "lint_lines: $f is within $LIMIT lines now: take it off $allow" >&2
    fail=1
  fi
done <"$allow"
[ "$fail" -eq 0 ] && echo "file sizes: within $LIMIT lines (or shrinking)"
exit "$fail"
