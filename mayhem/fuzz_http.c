/* Raw HTTP bytes through webdis' client pipeline: http_parser callbacks, URL/query parsing, ACL,
 * command routing, Redis round-trip against the fake Redis, and the JSON/raw/msgpack/custom-type
 * formatters. The low bit of the input size picks whether the client hangs up before or after Redis
 * has answered (a disconnect with a command in flight is its own code path in http_client_read). */
#include "harness.h"

int
LLVMFuzzerInitialize(int *argc, char ***argv) {
	(void)argc;
	(void)argv;
	harness_init(1);
	return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	int sv[2];
	struct http_client *c;
	int early_hangup = size & 1;
	size_t n = size > HARNESS_MAX_INPUT ? HARNESS_MAX_INPUT : size;

	g_reply = NULL;
	h_open_client(sv);
	c = http_client_new(g_w, sv[0], htonl(INADDR_LOOPBACK));
	worker_monitor_input(c);

	h_write_all(sv[1], data, n);
	h_pump(early_hangup ? 2 : h_passes(n));
	shutdown(sv[1], SHUT_WR);
	h_pump(h_passes(n));
	h_finish_client();
	return 0;
}
