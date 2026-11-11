# Known findings: fuzz_http

Reproducers found by this target during integration (replay with `/mayhem/fuzz_http-standalone <file>`). They are kept out of `testsuite/` on purpose.

## `http-close-uaf-cmd-free.bin`: heap-use-after-free read in `cmd_free`
- Cause: A request with `Connection: close`, HTTP/1.0 or no version marks the client `broken`; `worker_can_read` then calls `http_client_free(c)` while the Redis command is still in flight and `cmd->http_client` still points at it. When the reply arrives `cmd_free` reads `c->http_client->last_cmd`.
- Fix: Clear `cmd->http_client`/`last_cmd` (or defer `http_client_free`) before freeing a client that has a command in flight.

## `http-parser-global-overflow.bin`: global-buffer-overflow read in vendored `http_parser_execute`
- Cause: A malformed request line indexes a lookup table out of range in the old vendored http-parser.
- Fix: Update the vendored http-parser (the check on the upgraded table index exists upstream in nodejs/http-parser).

## `subscribe-nonkeepalive-uaf.bin`: heap-use-after-free write in `format_send_error` (`cmd->pub_sub_client->reused_cmd = NULL`)
- Cause: A SUBSCRIBE on a non-keep-alive HTTP request frees the `http_client` right after parsing, but the subscribe `cmd` keeps `pub_sub_client` pointing at it; when the Redis link ends (or a message arrives) webdis writes to freed memory.
- Fix: Do not free a client that owns a pub/sub command (or null `pub_sub_client` in `http_client_free`).

## `sds-memcpy-null.bin`: UBSan: null pointer passed to `memcpy` in `sdscatlen`
- Cause: `sdscatlen` (vendored hiredis) is called with a NULL source pointer from a command built out of an odd request path (the reproducer is a `GET /Upgrade/PSUBSCRIBE/` style request); UBSan halts on `memcpy(dst, NULL, n)`. Reduced by the fuzzer, root caller not analysed further.
- Fix: skip the `memcpy` when `t` is NULL / `len` is 0.

## `cmd-memcpy-null.bin`: UBSan: null pointer passed to `memchr` in `cmd_run`
- Cause: `cmd_run` calls `memchr(uri, '?', uri_len)` with a NULL `uri` for a POST whose URL path is not the command (the command is taken from the body); UBSan halts on the NULL argument.
- Fix: only search for `?` when `uri` is non-NULL.
