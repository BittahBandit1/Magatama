#!/bin/zsh -f
# Start the authorized October 25 session and release the sleep assertion at cutoff.
set -eu
repo="${0:A:h:h}"
if pgrep -f "$repo/build/magatama/bin/Fukami" >/dev/null; then
    print -u2 "Close the current Magatama game, then run this command again."
    exit 1
fi
stop_at="$(python3 - <<'PY'
from datetime import datetime
from zoneinfo import ZoneInfo
now = datetime.now(ZoneInfo('America/Chicago'))
end = datetime(2026, 10, 25, 8, 15, tzinfo=now.tzinfo)
if now >= end:
    raise SystemExit('This overnight session ended at October 25, 08:15 America/Chicago.')
print(int(end.timestamp()))
PY
)"
export MAGATAMA_AGENT_STOP_AT="$stop_at"
export MAGATAMA_AGENT_CARD_ONLY=1
print "Overnight runner stops October 25 at 8:15 AM Central. Keep this Terminal and Codex open."
exec caffeinate -dims "$repo/scripts/magatama_agent.sh"
