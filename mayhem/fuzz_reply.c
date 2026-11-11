/* Redis replies through webdis' reply formatters. The first input byte picks one of the requests
 * below (all use a /1/ database prefix so webdis opens a dedicated Redis connection, which a protocol
 * error may close without disturbing the pool); the rest is the exact byte stream the fake Redis sends
 * back for that request. hiredis parses it, then json_reply/raw_reply/msgpack_reply/custom_type_reply
 * and the HTTP response writer run on whatever it produced. */
#include "harness.h"

#define HDR " HTTP/1.1\r\nHost: localhost\r\n\r\n"
static const char *const requests[] = {
	"GET /1/GET/key.json" HDR,
	"GET /1/GET/key.raw" HDR,
	"GET /1/GET/key.msg" HDR,
	"GET /1/GET/key.txt" HDR,
	"GET /1/GET/key?type=text/html" HDR,
	"GET /1/GET/key.json?jsonp=cb" HDR,
	"GET /1/GET/key.js?callback=cb2" HDR,
	"GET /1/GET/key.png?filename=a.png" HDR,
	"GET /1/LRANGE/key/0/-1.json" HDR,
	"GET /1/LRANGE/key/0/-1.raw?sep=%2C" HDR,
	"GET /1/LRANGE/key/0/-1.msg" HDR,
	"GET /1/HGETALL/key.json" HDR,
	"GET /1/HGETALL/key.raw" HDR,
	"GET /1/HGETALL/key.msg" HDR,
	"GET /1/INFO.json" HDR,
	"GET /1/INFO.raw" HDR,
	"GET /1/MGET/a/b/c.json" HDR,
	"GET /1/ZRANGE/z/0/-1/WITHSCORES.json" HDR,
	"GET /1/SCAN/0.json" HDR,
	"GET /1/SUBSCRIBE/chan.json" HDR,
	"GET /1/PSUBSCRIBE/chan.raw" HDR,
	"GET /1/SUBSCRIBE/chan.msg" HDR,
	"POST /1/ HTTP/1.1\r\nHost: localhost\r\nContent-Length: 14\r\n\r\n1/GET/key.json",
	"PUT /1/SET/key.json HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nvalue",
};

int
LLVMFuzzerInitialize(int *argc, char ***argv) {
	(void)argc;
	(void)argv;
	harness_init(0);
	return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	int sv[2];
	struct http_client *c;
	const char *req;
	size_t n;

	if(size < 1) {
		return -1;
	}
	req = requests[data[0] % (sizeof(requests) / sizeof(requests[0]))];
	data++;
	size--;
	n = size > HARNESS_MAX_INPUT ? HARNESS_MAX_INPUT : size;

	g_reply = data;
	g_reply_len = h_first_reply_len(data, n);
	h_open_client(sv);
	c = http_client_new(g_w, sv[0], htonl(INADDR_LOOPBACK));
	worker_monitor_input(c);

	h_write_all(sv[1], req, strlen(req));
	h_pump(h_passes(n));
	shutdown(sv[1], SHUT_WR);
	h_pump(h_passes(n));
	h_finish_client();
	g_reply = NULL;
	return 0;
}
