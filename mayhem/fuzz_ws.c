/* WebSocket frames (RFC 6455) as they arrive after the handshake: ws_peek_data/ws_process_read_data,
 * the JSON and raw command extractors, ACL, and the framed replies. The first input byte picks the
 * endpoint the client connected to ("/", "/.json" or "/.raw", the three that select a message format),
 * the rest is the byte stream the client sends. The handshake itself is covered by fuzz_http. */
#include "harness.h"

#include <string.h>

int
LLVMFuzzerInitialize(int *argc, char ***argv) {
	(void)argc;
	(void)argv;
	harness_init(0);
	return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	static const char *const paths[] = {"/", "/.json", "/.raw"};
	int sv[2];
	struct http_client *c;
	struct ws_client *ws;
	const char *path;
	size_t n;

	if(size < 1) {
		return -1;
	}
	path = paths[data[0] % 3];
	data++;
	size--;
	n = size > HARNESS_MAX_INPUT ? HARNESS_MAX_INPUT : size;

	g_reply = NULL;
	h_open_client(sv);
	c = http_client_new(g_w, sv[0], htonl(INADDR_LOOPBACK));
	c->path = strdup(path);
	c->path_sz = strlen(path);
	c->is_websocket = 1;
	c->keep_alive = 1;
	c->ws = ws = ws_client_new(c);
	if(!ws) {
		close(sv[0]);
		free(c->path);
		free(c);
		h_finish_client();
		return 0;
	}
	ws_monitor_input(ws);

	h_write_all(sv[1], data, n);
	h_pump(h_passes(n));
	shutdown(sv[1], SHUT_WR);
	h_pump(h_passes(n));
	h_finish_client();
	return 0;
}
