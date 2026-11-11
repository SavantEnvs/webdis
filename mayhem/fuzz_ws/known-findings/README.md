# Known findings: fuzz_ws

Reproducers found by this target during integration (replay with `/mayhem/fuzz_ws-standalone <file>`). They are kept out of `testsuite/` on purpose.

## `raw-ws-incomplete-frame-null-deref.bin`: UBSan null deref in `raw_ws_extract`
- Cause: A WebSocket frame on a `.raw` endpoint carrying an incomplete RESP message makes `redisReaderGetReply` return OK with a NULL reply, which is dereferenced.
- Fix: Check `reply == NULL` after `redisReaderGetReply`.

## `ws-close-uaf.bin`: heap-use-after-free read in `json_wrap_redis_reply`
- Cause: A WebSocket close frame runs `ws_close_if_able`, freeing the `ws_client`, while a Redis command issued earlier is still pending; its reply callback then dereferences `cmd->http_client->ws`.
- Fix: Cancel or detach in-flight commands in `ws_client_free`.
