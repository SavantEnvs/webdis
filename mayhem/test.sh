#!/usr/bin/env bash
#
# mayhem/test.sh: RUN webdis' own functional suite (tests/basic.py) against the stock-flags webdis binary
# that mayhem/build.sh built, talking to a private redis-server. exit 0 = pass. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# The suite is webdis' own tests/basic.py: it issues real HTTP requests and asserts the exact JSON/raw
# bodies, content types and ETags. A webdis that does nothing (or exits 0) fails every one of them.
[ -x "$SRC/webdis" ] || { echo "missing $SRC/webdis: mayhem/build.sh should have built it" >&2; emit_ctrf "webdis-basic" 0 1 0; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/webdis-test-XXXXXX")"
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
SOCK="$WORK/redis.sock"

redis-server --port 0 --unixsocket "$SOCK" --unixsocketperm 700 --save "" --appendonly no \
  --dir "$WORK" --daemonize no >"$WORK/redis.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 100); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] || { cat "$WORK/redis.log" >&2; emit_ctrf "webdis-basic" 0 1 0; exit 1; }

cat > "$WORK/webdis.json" <<JSON
{"redis_host":"$SOCK","http_host":"127.0.0.1","http_port":$PORT,"threads":2,"pool_size":10,
 "database":0,"daemonize":false,"websockets":false,"verbosity":3,"logfile":"$WORK/webdis.log"}
JSON
"$SRC/webdis" "$WORK/webdis.json" >"$WORK/webdis.out" 2>&1 &
PIDS+=("$!")
for _ in $(seq 100); do
  python3 -c "import socket; socket.create_connection(('127.0.0.1',$PORT),0.2).close()" 2>/dev/null && break
  sleep 0.1
done

WEBDIS_HOST=127.0.0.1 WEBDIS_PORT="$PORT" timeout 600 python3 -m unittest -v tests/basic.py >"$WORK/unittest.log" 2>&1
cat "$WORK/unittest.log"

passed=$(grep -cE ' \.\.\. ok$' "$WORK/unittest.log")
failed=$(grep -cE ' \.\.\. (FAIL|ERROR)$' "$WORK/unittest.log")
skipped=$(grep -cE ' \.\.\. skipped' "$WORK/unittest.log")
# a crashed/killed suite prints no per-test lines: count that as one failure
[ $((passed + failed + skipped)) -gt 0 ] || failed=1
emit_ctrf "webdis-basic" "$passed" "$failed" "$skipped"
