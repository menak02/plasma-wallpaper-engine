#!/usr/bin/env bash
# Maintains the sticky repo-quality PR comment: exactly one comment per PR,
# found via a hidden HTML marker and edited in place by every run that
# reports. Failure runs refresh the findings; later passing runs flip the
# same comment to a clean status instead of leaving stale findings behind.
#
# Usage:
#   update_repo_quality_comment.sh report <report-file>   post/refresh findings
#   update_repo_quality_comment.sh clean                  clear an existing comment
#
# Required environment (set by CI):
#   GH_TOKEN, PR_NUMBER, RUN_URL, GITHUB_REPOSITORY, GITHUB_RUN_ID
set -euo pipefail

MODE="${1:-}"
REPORT_FILE="${2:-}"

if [ "$MODE" != "report" ] && [ "$MODE" != "clean" ]; then
  echo "usage: $0 report <report-file> | clean" >&2
  exit 2
fi
if [ "$MODE" = "report" ] && [ ! -f "$REPORT_FILE" ]; then
  echo "report file not found: $REPORT_FILE" >&2
  exit 2
fi

: "${GH_TOKEN:?GH_TOKEN must be set}"
: "${PR_NUMBER:?PR_NUMBER must be set}"
: "${RUN_URL:?RUN_URL must be set}"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"

MARKER="<!-- repo-quality-findings:v1 -->"
BODY_FILE="$(mktemp)"
trap 'rm -f "$BODY_FILE"' EXIT

STAMP="$(date -u '+%Y-%m-%d %H:%M UTC')"
RUN_REF="[run ${GITHUB_RUN_ID:-?}](${RUN_URL})"

if [ "$MODE" = "report" ]; then
  {
    echo "$MARKER"
    echo "**Repo-quality findings** — fix or add an allowlist keyword in \`scripts/check_repo_quality.sh\`:"
    echo '```'
    grep -v '^::error ' "$REPORT_FILE"
    echo '```'
    echo
    echo "_Updated by ${RUN_REF} at ${STAMP}._"
  } > "$BODY_FILE"
else
  {
    echo "$MARKER"
    echo "✅ **Repo-quality checks passed** — no scaffolding or generic-name findings."
    echo
    echo "_Cleared by ${RUN_REF} at ${STAMP}._"
  } > "$BODY_FILE"
fi

COMMENT_ID="$(gh api "repos/${GITHUB_REPOSITORY}/issues/${PR_NUMBER}/comments" \
  --jq ".[] | select(.body | contains(\"${MARKER}\")) | .id" | head -n1)"

if [ -n "$COMMENT_ID" ]; then
  gh api -X PATCH "repos/${GITHUB_REPOSITORY}/issues/comments/${COMMENT_ID}" \
    -F body=@"$BODY_FILE" > /dev/null
  echo "Updated existing comment ${COMMENT_ID} (${MODE})"
elif [ "$MODE" = "report" ]; then
  gh api -X POST "repos/${GITHUB_REPOSITORY}/issues/${PR_NUMBER}/comments" \
    -F body=@"$BODY_FILE" > /dev/null
  echo "Created new findings comment"
else
  echo "No sticky comment to clear; skipping"
fi
