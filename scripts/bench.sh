#!/bin/bash
# Task 6: time a 1 MiB transfer through the relay (--delay 50, default 250 ms
# timeout) for windows 1 and 16 at 0% and 5% loss, three runs each, checking
# every copy with cmp. Prints a Markdown table and saves the raw output under
# results/.
#
#   bash scripts/bench.sh            # relay path, port and runs can be overridden:
#   RELAY=/path/to/cs425_relay.py PORT=42510 RUNS=3 bash scripts/bench.sh
#
# Needs a release build (make release) and python3. Uses session names
# jakedickinson-w<window>-l<loss%>-r<run>.
set -u

cd "$(dirname "$0")/.." || exit 1
RELAY=${RELAY:-"/mnt/c/Users/12089/Desktop/Claude Folder/425_P2/cs425_relay.py"}
PORT=${PORT:-42510}
RUNS=${RUNS:-3}
APP=./build/release/myapp
SESSION_PREFIX=${SESSION_PREFIX:-jakedickinson}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=results/bench-$STAMP.txt
RELAY_LOG=results/relay-$STAMP.log
WORK=$(mktemp -d)

die() { echo "bench: $*" | tee -a "$OUT" >&2; exit 1; }

mkdir -p results
: > "$OUT"
[ -x "$APP" ] || die "no $APP: run 'make release' first"
[ -f "$RELAY" ] || die "relay not found at $RELAY (set RELAY=...)"

[ -f 1mib.bin ] || head -c 1048576 /dev/urandom > 1mib.bin
[ "$(stat -c %s 1mib.bin)" -eq 1048576 ] || die "1mib.bin is not 1 MiB"

python3 "$RELAY" --delay 50 --port "$PORT" > "$RELAY_LOG" 2>&1 &
RELAY_PID=$!
# SIGTERM, not SIGINT: a relay started in the background ignores SIGINT.
trap 'kill $RELAY_PID 2>/dev/null; rm -rf "$WORK"' EXIT
sleep 1
kill -0 $RELAY_PID 2>/dev/null || die "relay did not start (see $RELAY_LOG)"

{
  echo "Task 6 benchmark, $(date '+%Y-%m-%d %H:%M:%S')"
  echo "host: $(uname -srm); relay --delay 50 on 127.0.0.1:$PORT; timeout 250 ms (default);"
  echo "file: 1mib.bin (1048576 bytes = 1025 packets); $RUNS runs per combination; every copy checked with cmp"
  echo
} | tee -a "$OUT"

TIMEFORMAT=%R
declare -A TIMES STATS

for combo in "1 0" "16 0" "1 0.05" "16 0.05"; do
  set -- $combo
  w=$1 loss=$2
  pct=$(awk -v l="$loss" 'BEGIN { printf "%d", l * 100 + 0.5 }')
  for r in $(seq 1 "$RUNS"); do
    session=$SESSION_PREFIX-w$w-l$pct-r$r
    out=$WORK/$session.bin
    "$APP" recv -s "$session" -p "$PORT" 127.0.0.1 "$out" 2> "$WORK/$session.recv" &
    rpid=$!
    sleep 0.5
    t=$( { time "$APP" send -s "$session" -w "$w" -l "$loss" -p "$PORT" 127.0.0.1 1mib.bin \
            2> "$WORK/$session.send"; } 2>&1 )
    sx=$?
    wait $rpid
    rx=$?
    stats=$(sed -n 's/.*: \([0-9]*\) datagrams, \([0-9]*\) retransmitted, \([0-9]*\) timeouts/\1 \2 \3/p' \
            "$WORK/$session.send")
    same=identical
    cmp -s 1mib.bin "$out" || same=DIFFERENT
    printf '%-26s window %2s loss %-4s  %7.3f s  send=%d recv=%d  %s  sent/retx/timeouts: %s\n' \
      "$session" "$w" "$loss" "$t" "$sx" "$rx" "$same" "${stats:-?}" | tee -a "$OUT"
    if [ "$sx" -ne 0 ] || [ "$rx" -ne 0 ] || [ "$same" != identical ]; then
      cat "$WORK/$session.send" "$WORK/$session.recv" | tee -a "$OUT"
      die "run $session FAILED"
    fi
    TIMES[$w,$loss]="${TIMES[$w,$loss]:-} $t"
    STATS[$w,$loss]="${STATS[$w,$loss]:-}|$stats"
  done
done

{
  echo
  echo "| Window | Loss | Run 1 (s) | Run 2 (s) | Run 3 (s) | Mean (s) | Throughput (KiB/s) | Mean retransmitted | Mean timeouts |"
  echo "|---|---|---|---|---|---|---|---|---|"
  for combo in "1 0" "16 0" "1 0.05" "16 0.05"; do
    set -- $combo
    echo "$1 $2 ${TIMES[$1,$2]} ${STATS[$1,$2]}"
  done | awk '{
    split($0, parts, "|"); n = split(parts[1], f, " ")
    runs = ""; sum = 0
    for (i = 3; i <= n; i++) { runs = runs sprintf(" %.2f |", f[i]); sum += f[i] }
    mean = sum / (n - 2)
    rsum = 0; tsum = 0; k = 0
    for (j = 2; j in parts; j++) { split(parts[j], s, " "); rsum += s[2]; tsum += s[3]; k++ }
    printf "| %s | %s |%s %.2f | %.1f | %.0f | %.0f |\n", f[1], f[2], runs, mean, 1024 / mean, rsum / k, tsum / k
  }'
} | tee -a "$OUT"

# Let the relay log the last session summaries (it does so 30 s after a session goes idle).
sleep 31
echo "raw output: $OUT; relay log: $RELAY_LOG"
