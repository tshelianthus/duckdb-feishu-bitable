#!/usr/bin/env bash

# Reject Feishu/Lark credential-shaped values in version-controlled files.
# Keep examples visibly synthetic by using angle-bracket or XXXX placeholders.

set -euo pipefail

readonly SCRIPT_PATH="scripts/check_no_secrets.sh"
readonly -a SECRET_PATTERNS=(
  'cli_[a-z0-9]{16,}'
  'u-[A-Za-z0-9_-]{20,}'
  't-[A-Za-z0-9_-]{20,}'
  'bascn[A-Za-z0-9]{10,}'
  'tbl[A-Za-z0-9]{10,}'
)

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

grep_args=()
for pattern in "${SECRET_PATTERNS[@]}"; do
  grep_args+=("-e" "$pattern")
done

matches="$(git grep -IEno "${grep_args[@]}" -- . ":(exclude)$SCRIPT_PATH" || true)"
violations=""

while IFS= read -r match; do
  [[ -z "$match" ]] && continue
  candidate="${match#*:*}"
  candidate="${candidate#*:}"

  # Pure X placeholders are safe. These three explicit values are longstanding
  # test fixtures used to prove that errors and secret metadata are redacted.
  if [[ "$candidate" =~ ^(cli_|u-|t-|bascn|tbl)[xX]+$ ]]; then
    continue
  fi
  case "$candidate" in
    u-sensitive-user-token-value|u-preferred-user-token|t-should-never-appear-in-error)
      continue
      ;;
  esac

  violations+="$match"$'\n'
done <<< "$matches"

if [[ -n "$violations" ]]; then
  echo "Credential-shaped values found in version-controlled files:" >&2
  printf '%s' "$violations" >&2
  echo "Replace them with angle-bracket or XXXX placeholders." >&2
  exit 1
fi

echo "No credential-shaped values found in version-controlled files."
