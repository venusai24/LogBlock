#!/usr/bin/env bash
# Guards against the check-then-create race on a missing log (T043).
# Usage: scripts/check_create_guard.sh [dir...]     (default: src include)
# Exits non-zero, printing file:line, when either guard fails.
set -uo pipefail

if [[ $# -eq 0 ]]; then set -- src include; fi
status=0

# Name guard: production code must not call a create-without-O_EXCL helper. open_log() and
# exists() were removed from Io because a helper like that is how the race comes back.
if grep -rnE '\bopen_log\(|(->|\.)exists\(|\bIo::exists\b' "$@"; then
  echo "check_create_guard: open_log() or Io::exists() is used; startup must use" >&2
  echo "          open_log_existing() then create_log() (see T043)" >&2
  status=1
fi

# Flag guard: every line that uses O_CREAT must also contain O_EXCL, so a create can never
# silently follow an existing file. The rule is LINE-BASED: a flag list wrapped over two
# lines puts O_CREAT and O_EXCL on different lines and FAILS CLOSED (keep them on one line).
# Text after "//" is ignored, so comments that mention O_CREAT are fine.
while IFS= read -r hit; do
  # hit is "file:line:text"; strip the prefix, then any // comment, before looking at the code.
  code="${hit#*:*:}"
  code="${code%%//*}"
  if [[ "${code}" == *O_CREAT* && "${code}" != *O_EXCL* ]]; then
    echo "${hit%%:*}:$(cut -d: -f2 <<<"${hit}"): O_CREAT without O_EXCL on the same line" >&2
    status=1
  fi
done < <(grep -rn 'O_CREAT' "$@")

exit "${status}"
