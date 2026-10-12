#!/usr/bin/env bash
# Ask OpenAI Codex (ChatGPT) for a second opinion from inside this repository.
#
#   scripts/codex_ask.sh "explain how the NGX shim selects the native path"
#   git diff | scripts/codex_ask.sh "review this diff for bugs"
#   scripts/codex_ask.sh --review            # codex reviews the working tree changes
#
# Codex runs read-only by default; set CODEX_SANDBOX=workspace-write to let it edit files.
# Auth: OPENAI_API_KEY (env var or a network secret on api.openai.com), or an existing
# `codex login` / `codex login --device-auth` (ChatGPT account).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SANDBOX="${CODEX_SANDBOX:-read-only}"

if ! command -v codex >/dev/null 2>&1; then
  npm install -g @openai/codex >/dev/null
fi

if ! codex login status >/dev/null 2>&1; then
  if [[ -n "${OPENAI_API_KEY:-}" ]]; then
    printenv OPENAI_API_KEY | codex login --with-api-key >/dev/null
  else
    cat >&2 <<'EOF'
codex is not logged in. Either:
  - set OPENAI_API_KEY (environment variable or a network secret for api.openai.com), or
  - run `codex login --device-auth` to sign in with a ChatGPT account.
EOF
    exit 1
  fi
fi

if [[ "${1:-}" == "--review" ]]; then
  shift
  exec codex exec -C "$ROOT" -s "$SANDBOX" review "$@"
fi

exec codex exec -C "$ROOT" -s "$SANDBOX" "$@"
