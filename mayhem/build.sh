#!/usr/bin/env bash
#
# mayhem/build.sh: build webdis three ways.
#   1. sanitized library objects (ASan+UBSan, halting, DWARF 3) in mayhem/build/obj, linked into each
#      in-process harness (fuzz_http, fuzz_ws, fuzz_reply) plus a -standalone reproducer of each
#   2. the stock `make` build (upstream's own flags) -> /mayhem/webdis, which mayhem/test.sh runs against
#      a real redis-server
# Everything it needs comes from apt packages baked into the image, so it also runs with no network.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty): it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

B=mayhem/build
mkdir -p "$B/obj"

MSGPACK_CFLAGS="$(pkg-config --cflags msgpack-c)"
MSGPACK_LIBS="$(pkg-config --libs msgpack-c)"

# 1) sanitized library: every webdis source except src/webdis.c (main)
SRCS=(
  src/acl.c src/client.c src/cmd.c src/conf.c src/http.c src/pool.c src/server.c src/slog.c
  src/websocket.c src/worker.c
  src/formats/common.c src/formats/custom-type.c src/formats/json.c src/formats/raw.c src/formats/msgpack.c
  src/b64/cencode.c src/md5/md5.c src/sha1/sha1.c src/http-parser/http_parser.c
  src/hiredis/hiredis.c src/hiredis/sds.c src/hiredis/net.c src/hiredis/async.c src/hiredis/read.c
  src/hiredis/dict.c src/hiredis/alloc.c src/hiredis/sockcompat.c
  src/jansson/src/dump.c src/jansson/src/error.c src/jansson/src/hashtable.c src/jansson/src/hashtable_seed.c
  src/jansson/src/load.c src/jansson/src/memory.c src/jansson/src/pack_unpack.c src/jansson/src/strbuffer.c
  src/jansson/src/strconv.c src/jansson/src/utf.c src/jansson/src/value.c
)
INC="-Isrc -Isrc/jansson/src -Isrc/http-parser"
LIB_CFLAGS="$SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS -O1 -std=c99 -D_POSIX_C_SOURCE=200809L -Wno-pragmas -DMSGPACK=1 $MSGPACK_CFLAGS $INC"
export LIB_CFLAGS B
compile_one() { "$CC" $LIB_CFLAGS -c "$1" -o "$B/obj/$(echo "${1%.c}" | tr / _).o"; }
export -f compile_one
printf '%s\n' "${SRCS[@]}" | xargs -P "$MAYHEM_JOBS" -I{} bash -c 'compile_one {}'

# 2) harnesses, each as a libFuzzer binary and as a run-once standalone reproducer
"$CXX" $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o "$B/lsan_off.o"
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$B/standalone_main.o"
for t in fuzz_http fuzz_ws fuzz_reply; do
  "$CC" $SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS -O1 $INC -Imayhem \
      -c "mayhem/$t.c" -o "$B/$t.o"
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$B/$t.o" "$B"/obj/*.o "$B/lsan_off.o" \
      -levent -pthread $MSGPACK_LIBS -o "$SRC/$t"
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS "$B/standalone_main.o" "$B/$t.o" "$B"/obj/*.o "$B/lsan_off.o" \
      -levent -pthread $MSGPACK_LIBS -o "$SRC/$t-standalone"
done

# 3) upstream's stock build with its own flags (no sanitizers): the oracle in mayhem/test.sh runs it
env -u CFLAGS -u LDFLAGS make -j"$MAYHEM_JOBS" CC="$CC $COVERAGE_FLAGS" webdis
