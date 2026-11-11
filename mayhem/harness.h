/*
 * Shared in-process harness for webdis.
 *
 * One webdis worker is built by hand (the same structs server_new()/worker_new() build) around one
 * libevent base, and a fake Redis listens on a unix socket in $TMPDIR. The fake Redis speaks real
 * RESP: it parses every command with hiredis' own reader and answers each one exactly once, so the
 * pooled connections stay healthy from one input to the next. Webdis' event callbacks are driven by
 * pumping the base (no threads, no timers): the fuzzed bytes arrive on a socketpair exactly like
 * bytes from a TCP client, and EOF on that socket makes webdis free the client the way it does in
 * production.
 */
#ifndef WEBDIS_FUZZ_HARNESS_H
#define WEBDIS_FUZZ_HARNESS_H

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <event2/event.h>
#include <hiredis/hiredis.h>

#include "client.h"
#include "cmd.h"
#include "conf.h"
#include "pool.h"
#include "server.h"
#include "websocket.h"
#include "worker.h"

#define HARNESS_MAX_INPUT (1 << 16)

struct fconn {
	int fd;
	struct event *ev;
	redisReader *rd;
	int persistent;
	struct fconn *next;
};

static struct event_base *g_base;
static struct worker *g_w;
static struct fconn *g_conns;
static int g_listen_fd = -1;
static struct event *g_listen_ev;
static int g_peer = -1; /* fuzzer's end of the client socketpair */
static char g_dir[256], g_sock[320], g_conf[320];

/* NULL: canned, command-aware replies; otherwise this payload answers every non-SELECT command */
static const uint8_t *g_reply;
static size_t g_reply_len;

static void
h_nonblock(int fd) {
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void
h_write_all(int fd, const void *p, size_t n) {
	const char *c = p;
	while(n) {
		ssize_t r = write(fd, c, n);
		if(r <= 0) {
			break;
		}
		c += r;
		n -= (size_t)r;
	}
}

static void
fconn_close(struct fconn *fc) {
	struct fconn **pp;
	for(pp = &g_conns; *pp; pp = &(*pp)->next) {
		if(*pp == fc) {
			*pp = fc->next;
			break;
		}
	}
	event_free(fc->ev);
	close(fc->fd);
	redisReaderFree(fc->rd);
	free(fc);
}

static int
h_is(const redisReply *r, const char *name) {
	return r->type == REDIS_REPLY_ARRAY && r->elements > 0 &&
		r->element[0]->type == REDIS_REPLY_STRING && strcasecmp(r->element[0]->str, name) == 0;
}

static void
canned_reply(int fd, const redisReply *cmd) {
	const char *out;
	if(h_is(cmd, "GET")) {
		out = "$5\r\nworld\r\n";
	} else if(h_is(cmd, "INCR")) {
		out = ":1\r\n";
	} else if(h_is(cmd, "LRANGE") || h_is(cmd, "MGET") || h_is(cmd, "SMEMBERS")) {
		out = "*2\r\n$3\r\nabc\r\n$3\r\ndef\r\n";
	} else if(h_is(cmd, "HGETALL")) {
		out = "*4\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$-1\r\n";
	} else if(h_is(cmd, "INFO")) {
		out = "$54\r\n# Server\r\nredis_version:7.0.0\r\nuptime_in_seconds:42\r\n\r\n";
	} else if(h_is(cmd, "PING")) {
		out = "+PONG\r\n";
	} else if(h_is(cmd, "SUBSCRIBE") || h_is(cmd, "PSUBSCRIBE")) {
		out = "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n";
	} else if(h_is(cmd, "EXISTS") || h_is(cmd, "DEL") || h_is(cmd, "PUBLISH")) {
		out = ":0\r\n";
	} else if(h_is(cmd, "SET") || h_is(cmd, "FLUSHALL")) {
		out = "+OK\r\n";
	} else if(h_is(cmd, "ECHO") && cmd->elements > 1 && cmd->element[1]->type == REDIS_REPLY_STRING) {
		char hdr[32];
		int n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", cmd->element[1]->len);
		h_write_all(fd, hdr, (size_t)n);
		h_write_all(fd, cmd->element[1]->str, cmd->element[1]->len);
		h_write_all(fd, "\r\n", 2);
		return;
	} else {
		out = "-ERR unknown command\r\n";
	}
	h_write_all(fd, out, strlen(out));
}

static void
fconn_read(evutil_socket_t fd, short ev, void *arg) {
	struct fconn *fc = arg;
	char buf[4096];
	void *reply = NULL;
	ssize_t n;
	(void)ev;

	n = read(fd, buf, sizeof(buf));
	if(n < 0 && errno == EAGAIN) {
		return;
	}
	if(n <= 0) {
		fconn_close(fc);
		return;
	}
	redisReaderFeed(fc->rd, buf, (size_t)n);
	while(redisReaderGetReply(fc->rd, &reply) == REDIS_OK && reply) {
		const redisReply *cmd = reply;
		if(h_is(cmd, "SELECT") || h_is(cmd, "AUTH")) {
			h_write_all(fd, "+OK\r\n", 5);
		} else if(g_reply) {
			h_write_all(fd, g_reply, g_reply_len);
		} else {
			canned_reply(fd, cmd);
		}
		freeReplyObject(reply);
		reply = NULL;
	}
}

static void
listen_cb(evutil_socket_t fd, short ev, void *arg) {
	(void)ev;
	(void)arg;
	for(;;) {
		struct fconn *fc;
		int cfd = accept(fd, NULL, NULL);
		if(cfd < 0) {
			break;
		}
		h_nonblock(cfd);
		fc = calloc(1, sizeof(*fc));
		fc->fd = cfd;
		fc->rd = redisReaderCreate();
		fc->ev = event_new(g_base, cfd, EV_READ | EV_PERSIST, fconn_read, fc);
		event_add(fc->ev, NULL);
		fc->next = g_conns;
		g_conns = fc;
	}
}

/* Parse the decimal length line at p[*i]; advance *i past its CRLF. 0 if the line is not complete. */
static int
h_resp_num(const uint8_t *p, size_t n, size_t *i, long long *out) {
	size_t j = *i;
	int neg = 0, digits = 0;
	long long v = 0;
	if(j < n && (p[j] == '-' || p[j] == '+')) {
		neg = p[j] == '-';
		j++;
	}
	for(; j < n && p[j] >= '0' && p[j] <= '9'; ++j, ++digits) {
		if(v > (1LL << 40)) {
			return 0; /* cannot fit in the input anyway */
		}
		v = v * 10 + (p[j] - '0');
	}
	if(!digits || j + 1 >= n || p[j] != '\r' || p[j + 1] != '\n') {
		return 0;
	}
	*out = neg ? -v : v;
	*i = j + 2;
	return 1;
}

/* Length of the first complete RESP reply in p (n if it never completes or is malformed): a Redis
 * server answers a command once, and hiredis aborts on a second, unsolicited reply. This is a plain
 * byte scanner on purpose: it never runs hiredis' reader on the raw payload, so every parse of the
 * fuzzed bytes (and every assert in it) happens inside webdis' own connection. It only counts how
 * many items are still owed: an aggregate adds its element count (maps twice that), a null (-1)
 * aggregate or bulk adds nothing. */
static size_t
h_first_reply_len(const uint8_t *p, size_t n) {
	size_t i = 0;
	unsigned long long pending = 1;
	long long v;

	while(pending) {
		uint8_t t;
		if(i >= n) {
			return n;
		}
		t = p[i++];
		switch(t) {
		case '+': case '-': case ':': case ',': case '(': case '#': case '_':
			while(i + 1 < n && !(p[i] == '\r' && p[i + 1] == '\n')) {
				i++;
			}
			if(i + 1 >= n) {
				return n;
			}
			i += 2;
			pending--;
			break;
		case '$': case '=': case '!':
			if(!h_resp_num(p, n, &i, &v)) {
				return n;
			}
			if(v >= 0) {
				if((unsigned long long)v + 2 > n - i) {
					return n;
				}
				i += (size_t)v + 2;
			}
			pending--;
			break;
		case '*': case '~': case '>': case '%':
			if(!h_resp_num(p, n, &i, &v) || v < -1) {
				return n;
			}
			pending--;
			if(v > 0) {
				if((unsigned long long)v > n) {
					return n; /* more elements than the input has bytes */
				}
				pending += (unsigned long long)v * (t == '%' ? 2 : 1);
			}
			break;
		default:
			return n;
		}
	}
	return i;
}

static void
h_drain_peer(void) {
	char sink[4096];
	if(g_peer >= 0) {
		while(read(g_peer, sink, sizeof(sink)) > 0) {
		}
	}
}

static void
h_pump(int passes) {
	int i;
	for(i = 0; i < passes; ++i) {
		event_base_loop(g_base, EVLOOP_NONBLOCK);
		h_drain_peer();
	}
}

static int
h_passes(size_t input_len) {
	return 24 + (int)(input_len / 1024);
}

static void
h_cleanup(void) {
	unlink(g_sock);
	unlink(g_conf);
	rmdir(g_dir);
}

/* connect_pool: fill the worker's Redis pool (HTTP GETs without a /<db>/ prefix use it) */
static void
harness_init(int connect_pool) {
	struct sockaddr_un sa;
	struct server *s;
	FILE *f;
	int i;
	const char *tmp = getenv("TMPDIR");

	signal(SIGPIPE, SIG_IGN);
	if(!tmp || !*tmp) {
		tmp = "/tmp";
	}
	snprintf(g_dir, sizeof(g_dir), "%s/webdis-fuzz-XXXXXX", tmp);
	if(!mkdtemp(g_dir)) {
		perror("mkdtemp");
		exit(1);
	}
	snprintf(g_sock, sizeof(g_sock), "%s/redis.sock", g_dir);
	snprintf(g_conf, sizeof(g_conf), "%s/webdis.json", g_dir);
	atexit(h_cleanup);

	f = fopen(g_conf, "w");
	if(!f) {
		perror("fopen");
		exit(1);
	}
	fprintf(f,
		"{\"redis_host\":\"%s\",\"http_host\":\"127.0.0.1\",\"http_port\":0,"
		"\"threads\":1,\"pool_size\":2,\"websockets\":true,\"database\":0,"
		"\"http_max_request_size\":6000,\"verbosity\":8,\"logfile\":\"/dev/null\","
		"\"default_root\":\"/GET/default.json\","
		"\"acl\":[{\"disabled\":[\"DEBUG\",\"FLUSHALL\"]},"
		"{\"http_basic_auth\":\"user:password\",\"enabled\":[\"DEBUG\"]},"
		"{\"ip\":\"127.0.0.0/8\",\"disabled\":[\"CONFIG\"]}]}",
		g_sock);
	fclose(f);

	g_base = event_base_new();

	g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", g_sock);
	if(g_listen_fd < 0 || bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	   listen(g_listen_fd, 128) != 0) {
		perror("fake redis");
		exit(1);
	}
	h_nonblock(g_listen_fd);
	g_listen_ev = event_new(g_base, g_listen_fd, EV_READ | EV_PERSIST, listen_cb, NULL);
	event_add(g_listen_ev, NULL);

	s = calloc(1, sizeof(*s));
	s->log.fd = -1;
	s->cfg = conf_read(g_conf);
	slog_init(s);
	pthread_mutex_init(&s->auth_log_mutex, NULL);
	s->base = g_base;
	s->w = calloc(1, sizeof(struct worker *));
	g_w = worker_new(s);
	g_w->base = g_base;
	s->w[0] = g_w;

	if(connect_pool) {
		for(i = 0; i < g_w->pool->count; ++i) {
			pool_connect(g_w->pool, s->cfg->database, 1);
		}
		h_pump(32);
	}
	{
		struct fconn *fc;
		for(fc = g_conns; fc; fc = fc->next) {
			fc->persistent = 1;
		}
	}
}

/* Open the client socketpair; sv[0] is webdis' end, sv[1] is ours. */
static void
h_open_client(int sv[2]) {
	if(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		perror("socketpair");
		exit(1);
	}
	h_nonblock(sv[0]);
	h_nonblock(sv[1]);
	g_peer = sv[1];
}

/* Drop every Redis connection opened for this input (pub/sub, WS, custom DB) and the client's peer. */
static void
h_finish_client(void) {
	struct fconn *fc, *next;
	for(fc = g_conns; fc; fc = next) {
		next = fc->next;
		if(!fc->persistent) {
			fconn_close(fc);
		}
	}
	h_pump(8);
	if(g_peer >= 0) {
		close(g_peer);
		g_peer = -1;
	}
	h_pump(2);
}

#endif
