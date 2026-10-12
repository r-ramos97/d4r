# CLAUDE.md

## Codex (ChatGPT) integration

`scripts/codex_ask.sh` runs OpenAI Codex non-interactively against this repository, so Claude
can ask it for a second opinion, a code review, or a parallel investigation:

```bash
scripts/codex_ask.sh "where is the DLSS quality mode mapped to a render scale?"
git diff | scripts/codex_ask.sh "review this diff for correctness bugs"
scripts/codex_ask.sh --review
```

- Codex runs read-only unless `CODEX_SANDBOX=workspace-write` is set. Treat its answers as a
  second opinion to verify, not as ground truth.
- It installs `@openai/codex` with npm if missing, and logs in from `OPENAI_API_KEY` when set.
  Without a key, sign in with a ChatGPT account via `codex login --device-auth`.
- Cloud sessions need `api.openai.com` (and `auth.openai.com` for ChatGPT login) allowed in the
  environment's network access, otherwise every call fails with a proxy 403.
