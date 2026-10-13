#!/usr/bin/env bash
# The one tracking issue of the nightly esphome-upstream-dev job: opened when ESPHome's dev-branch
# gates fail on the hisense_ac component, rewritten each night they still fail, and closed with a
# comment the first night they pass. Without it a failure was a warning annotation on a green
# run, which nobody saw.
#
#   upstream-dev-issue.sh fail <report.md>   the failing gates, from upstream_gate_report.py
#   upstream-dev-issue.sh pass
#
# The issue is found by its title among the open issues, so there is never a second one. From
# the environment: GH_TOKEN (needs issues: write), GH_REPO, and for the text RUN_URL, REF_NAME
# (this repo's ref) and UPSTREAM (the commit of their dev branch).
set -euo pipefail

TITLE="Nightly: hisense_ac fails ESPHome's dev-branch gates"
result="${1:-}"
report="${2:-}"
case "$result" in
  fail) [ -f "$report" ] || { echo "usage: $0 fail <report.md>" >&2; exit 2; } ;;
  pass) ;;
  *) echo "usage: $0 fail <report.md> | pass" >&2; exit 2 ;;
esac

# The listing, not the search: the search index lags, and a lagging answer would open a second issue.
number="$(TITLE="$TITLE" gh issue list --state open --limit 1000 --json number,title \
  --jq '[.[] | select(.title == env.TITLE)][0].number // empty')"
where="${RUN_URL:-the run} (this repo at \`${REF_NAME:-?}\`, ESPHome \`dev\` at \`${UPSTREAM:-?}\`)"

if [ "$result" = pass ]; then
  if [ -z "$number" ]; then
    echo "the gates pass and no tracking issue is open: nothing to do"
    exit 0
  fi
  gh issue close "$number" --reason completed \
    --comment "The component passes ESPHome's dev-branch gates again: $where. Closing. The nightly job opens a new issue if they fail again."
  echo "closed tracking issue #$number"
  exit 0
fi

body="$(mktemp)"
trap 'rm -f "$body"' EXIT
{
  cat <<EOF
The nightly job \`esphome-upstream-dev\` runs ESPHome's own CI gates (\`firmware/scripts/esphome-upstream-check.sh\`) on the \`hisense_ac\` component, placed in a checkout of their \`dev\` branch. They fail there.

This does not block anything. The release this repo builds with is checked by the \`esphome-upstream\` job of the QA workflow, which gates pull requests and passes. What fails here is what an upstream pull request, or a move to a newer ESPHome release, would have to change first.

- Last failing run: $where
- Checked on: $(date -u +%Y-%m-%d) (UTC)

The job rewrites this text every night the gates fail and closes the issue the first night they pass.

## Failing gates

EOF
  cat "$report"
} > "$body"

if [ -n "$number" ]; then
  gh issue edit "$number" --body-file "$body" > /dev/null
  echo "updated tracking issue #$number"
else
  gh issue create --title "$TITLE" --body-file "$body"
fi
