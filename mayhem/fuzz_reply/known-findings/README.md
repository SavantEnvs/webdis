# Known findings: fuzz_reply

Reproducers found by this target during integration (replay with `/mayhem/fuzz_reply-standalone <file>`). They are kept out of `testsuite/` on purpose.

Every finding below crashes in webdis' own Redis connection (`redisLibeventHandler` -> `redisProcessCallbacks` -> `redisGetReply` or the pub/sub callback lookup in `async.c`), not in the harness: the harness only cuts the payload into the first RESP reply with a hand-written byte scanner (`h_first_reply_len` in `mayhem/harness.h`) and never runs hiredis' reader on the raw bytes.

## `hiredis-subscribe-assert-elements.bin`: `assert(reply->elements >= 2)` in hiredis `__redisGetSubscribeCallback`
- Cause: A Redis server (or MITM) sends a malformed pub/sub push; the vendored hiredis aborts instead of returning an error.
- Fix: Return an error from `__redisGetSubscribeCallback` instead of asserting (fixed in newer hiredis).

## `hiredis-bool-object-assert.bin`: `assert(parent->type == ARRAY||SET||MAP)` in `createBoolObject`
- Cause: a RESP3 `#` boolean nested in a `>` push. webdis' vendored hiredis `assert`s that the parent is an array, map or set, but a push (`>`) is a valid parent for `moveToNextTask`, so a hostile Redis server aborts webdis.
- Fix: Return an error instead of asserting.

## `hiredis-nil-object-assert.bin`: `assert(parent->type == ARRAY||...)` in `createNilObject`
- Cause: a RESP3 nil (`$-1`) nested in a `>` push. Same missing `REDIS_REPLY_PUSH` case in the parent-type assert as above.
- Fix: Return an error instead of asserting.

## `hiredis-subscribe-assert-type0.bin`: `assert(reply->element[0]->type == STRING)` in hiredis `__redisGetSubscribeCallback`
- Cause: Same class: pub/sub push whose first element is not a string.
- Fix: Same as above.

## `hiredis-subscribe-assert-type1.bin`: `assert(reply->element[1]->type == ...)` in hiredis `__redisGetSubscribeCallback`
- Cause: Same class: pub/sub push with a wrong second element type.
- Fix: Same as above.

## Huge aggregate counts (OOM)
- Cause: a reply such as `*4292483648\r\n` makes hiredis `calloc` the whole element array up front (about 32 GB), which libFuzzer reports as `out-of-memory`; a standalone run keeps spinning on it. No reproducer is stored because the standalone driver does not terminate on it.
- Fix: cap the aggregate length in the reader (newer hiredis has `maxelements`; webdis never sets it).
