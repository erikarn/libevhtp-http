#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>

#include <pthread.h>
#include <pthread_np.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <evhtp.h>

#define	debug_printf(...)
//#define	debug_printf(...) fprintf(stderr, __VA_ARGS__)

typedef enum {
	REQ_TYPE_NONE,
	REQ_TYPE_LINE,
	REQ_TYPE_SIZE,
} req_type_t;

struct http_app {
	int port;
	int ncpu;
	evbase_t *evbase;
	evhtp_t *htp;
};

struct thr {
	struct http_app *app;
	int t_tid;		/* thread id; local */
	int t_listen_fd;	/* listen_fd, or -1 for "we just asked evhtp for one */
	int t_our_fd;		/* 1 if the listen_fd is ours to use */
	pthread_t t_thr;
	evbase_t *t_evbase;
	evhtp_t *t_htp;
};

struct req {
	evhtp_request_t *req;

	req_type_t req_type;

	/*
	 * When generating sized-based replies; this
	 * will track how much more data needs to be
	 * written.
	 */
	size_t reply_size;
	off_t current_ofs;

	/*
	 * when generating the silly line-based replies; this
	 * will track how many more lines need to be written.
	 */
	int cur_count;
	int max_count;

	char *buf;
	size_t buf_size;

	/* Refcount = only free when the refcnt == 0 */
	int refcnt;
};

static struct req *
req_create(evhtp_request_t *req)
{
	struct req *r;

	r = malloc(sizeof(*r));
	if (r == NULL) {
		warn("%s: malloc", __func__);
		return (NULL);
	}

	r->req_type = REQ_TYPE_NONE;
	r->req = req;
	r->refcnt = 1;
	r->buf = NULL;

	return r;
}

static void
req_free(struct req *r)
{

	debug_printf("%s: %p: called; refcount=%d\n", __func__, r, r->refcnt);
	r->refcnt --;
	if (r->refcnt > 0)
		return;

	debug_printf("%s: %p: called; freeing\n", __func__, r);
	if (r->buf)
		free(r->buf);
	free(r);
}

static int
req_set_type_line(struct req *r, int max_count)
{

	r->req_type = REQ_TYPE_LINE;
	r->max_count = max_count;
	r->cur_count = 0;

	return (0);
}

static int
req_set_type_buf(struct req *r, size_t reply_size)
{
	off_t i;
	int j;

	r->buf = malloc(65536);
	if (r->buf == NULL) {
		warn("%s: %p: malloc", __func__, r);
		return (-1);
	}
	r->buf_size = 65536;
	r->req_type = REQ_TYPE_SIZE;
	r->reply_size = reply_size;
	r->current_ofs = 0;

#if 1
	/*
	 * To make life easier; let's put \n's after
	 * every 40 characters.
	 */
	for (i = 0, j = 0; i < r->buf_size - 1; i++) {
		if (i % 41 == 0) {
			r->buf[i] = '\n';
		} else {
			r->buf[i] = 'A' + (j % 26);
			j++;
		}
	}
	r->buf[r->buf_size - 1] = '\n';
#else
	for (i = 0; i < r->buf_size; i++) {
		r->buf[i] = 'A' + (i % 26);
	}
#endif

	return (0);
}

static int
req_write_line(struct req *r)
{
	struct evbuffer *evb;

	evb = evbuffer_new();
	/* XXX free callback - not needed; this is a static buffer */
	evbuffer_add_reference(evb, "foobar\r\n", 8, NULL, NULL);
	evhtp_send_reply_chunk(r->req, evb);
	evbuffer_free(evb);

	r->cur_count++;
	if (r->cur_count >= r->max_count) {
		evhtp_unset_hook(&r->req->conn->hooks, evhtp_hook_on_write);
		evhtp_send_reply_chunk_end(r->req);
		/* This will wrap up the connection for us via the fini path */
	}

	return (EVHTP_RES_OK);
}

static void
req_evbuf_free(const void *data, size_t datalen, void *arg)
{
	struct req *r = arg;

	req_free(r);
}

static int
req_write_buf(struct req *r)
{
	struct evbuffer *evb;
	size_t write_size;

	/* Figure out how much data we need to write */
	write_size = r->reply_size - r->current_ofs;
	if (write_size > r->buf_size)
		write_size = r->buf_size;

	evb = evbuffer_new();
	evbuffer_add_reference(evb, r->buf, write_size, req_evbuf_free, r);
	/* XXX methodize refcounting! */
	r->refcnt++;
	evhtp_send_reply_chunk(r->req, evb);
	evbuffer_free(evb);

	r->current_ofs += write_size;
	if (r->current_ofs >= r->reply_size) {
		evhtp_unset_hook(&r->req->conn->hooks, evhtp_hook_on_write);
		evhtp_send_reply_chunk_end(r->req);
		/* This will wrap up the connection for us via the fini path */
	}

	return (EVHTP_RES_OK);
}

static evhtp_res
send_upstream_new_chunk(evhtp_request_t * upstream_req, uint64_t len, void * arg)
{
	struct req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);
	return EVHTP_RES_OK;
}

evhtp_res
send_upstream_chunk_done(evhtp_request_t * upstream_req, void * arg)
{
	struct req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);
	return EVHTP_RES_OK;
}

evhtp_res
send_upstream_chunks_done(evhtp_request_t * upstream_req, void * arg)
{
	struct req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);
	return (EVHTP_RES_OK);
}

/*
 * Called upon socket error.
 */
evhtp_res
send_upstream_error(evhtp_request_t * req, evhtp_error_flags errtype, void * arg)
{
	struct req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);
	evhtp_unset_all_hooks(&req->hooks);
	r->req = NULL;
	req_free(r);

	return (EVHTP_RES_OK);
}

/*
 * This is called whenever the underlying HTTP write has made
 * some progress.
 *
 * TODO: we should be more selective about when we write data
 * by checking how much data is in the outbound write buffer.
 */
evhtp_res
send_upstream_on_write(evhtp_connection_t * conn, void * arg)
{
	struct req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);

	switch (r->req_type) {
	case REQ_TYPE_LINE:
		return req_write_line(r);
	case REQ_TYPE_SIZE:
		return req_write_buf(r);
	default:
		fprintf(stderr, "%s: %p: invalid type (%d)\n",
		    __func__, r, r->req_type);
		return (EVHTP_RES_ERROR);
	}
}

/*
 * Request: finished - error and non-error
 */
evhtp_res
send_upstream_fini(evhtp_request_t * upstream_req, void * arg)
{
	struct req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);

	evhtp_unset_all_hooks(&r->req->hooks);
	r->req = NULL;
	req_free(r);

	return (EVHTP_RES_OK);
}

static void
req_start_response(struct req *r)
{
	struct evbuffer *evb;

	/*
	 * Start an "OK" response for now.
	 *
	 * Register a callback to get notified when we've
	 * written some data.
	 */

	/* XXX not used for now - these are for receiving data */
	evhtp_set_hook(&r->req->hooks, evhtp_hook_on_new_chunk,
	    send_upstream_new_chunk, r);
	evhtp_set_hook(&r->req->hooks, evhtp_hook_on_chunk_complete,
	    send_upstream_chunk_done, r);
	evhtp_set_hook(&r->req->hooks, evhtp_hook_on_chunks_complete,
	    send_upstream_chunks_done, r);

	/* XXX for receiving or transmitting? */
	evhtp_set_hook(&r->req->hooks, evhtp_hook_on_error,
	    (evhtp_hook) send_upstream_error, r);

	/* write readiness - request? reply?  both */
	evhtp_set_hook(&r->req->conn->hooks, evhtp_hook_on_write,
	    send_upstream_on_write, r);

	/* Finished */
	evhtp_set_hook(&r->req->hooks, evhtp_hook_on_request_fini,
	    send_upstream_fini, r);

	/* XXX timeout? */

	/* .. ok, start the reply */

	switch (r->req_type) {
	case REQ_TYPE_LINE:
		evhtp_send_reply_chunk_start(r->req, EVHTP_RES_OK);
		req_write_line(r);
		return;
	case REQ_TYPE_SIZE:
		evhtp_send_reply_chunk_start(r->req, EVHTP_RES_OK);
		req_write_buf(r);
		return;
	default:
		fprintf(stderr, "%s: %p: invalid type (%d)\n",
		    __func__, r, r->req_type);
		evhtp_send_reply(r->req, EVHTP_RES_ERROR);
		return;
	}
}

void
linecb(evhtp_request_t * req, void * a)
{
	struct req *r;

	r = req_create(req);
	if (r == NULL) {
		/* XXX need to signal error; close connection */
		evhtp_send_reply(req, EVHTP_RES_ERROR);
		return;
	}
	/* XXX for now, type 'line' */
	req_set_type_line(r, 16);
	req_start_response(r);
}

void
sizecb(evhtp_request_t * req, void * a)
{
	struct req *r;
	evhtp_query_t *q;
	evhtp_kv_t *f;
	size_t reqsize;

	q = req->uri->query;

	/* Search the query string for a size parameter */
	f = evhtp_kvs_find_kv(q, "size");
	if (f == NULL) {
		evhtp_send_reply(req, EVHTP_RES_ERROR);
		return;
	}

	reqsize = strtoull(f->val, NULL, 10);

	/* Again, default to 128k for now */
	if (reqsize == ULLONG_MAX) {
		evhtp_send_reply(req, EVHTP_RES_ERROR);
		return;
	}

	r = req_create(req);
	if (r == NULL) {
		evhtp_send_reply(req, EVHTP_RES_ERROR);
		return;
	}

	req_set_type_buf(r, reqsize);

	req_start_response(r);
}

#if 0
static int
thr_setup(struct thr *th, int tid)
{
	th->t_listen_fd = -1;
	th->t_our_fd = 0;
	th->t_tid = tid;
	th->t_evbase = event_base_new();
	th->t_htp = evhtp_new(th->ev_base, NULL);

	return (0);
}
#endif

void
sighdl_pipe(int s)
{
}

static void
http_init_thread(evhtp_t *http, evthr_t *thread, void *arg)
{

	/*
	 * Nothing to do; there's no app base or per-thread
	 * state just yet.
	 */
	printf("%s: called\n", __func__);
}

static void
usage(const char *progname)
{
	printf("%s: --listen-port=<port> --number-threads=<numthr>\n",
	    progname);
	return;
}

enum {
	OPT_PORT = 1000,
	OPT_NUMBER_THREADS,
	OPT_HELP,
};

static struct option longopts[] = {
	{ "listen-port", required_argument, NULL, OPT_PORT },
	{ "number-threads", required_argument, NULL, OPT_NUMBER_THREADS },
	{ "help", no_argument, NULL, OPT_HELP },
	{ NULL, 0, NULL, 0 },
};

static int
parse_opts(struct http_app *app, int argc, char *argv[])
{
	int ch;

	while ((ch = getopt_long(argc, argv, "h", longopts, NULL)) != 1) {
		/*
		 * XXX I'm tired; ch shouldn't be -1 as above. Grr.
		 */
		if (ch == -1)
			break;

		switch (ch) {
		case OPT_PORT:
			app->port = atoi(optarg);
			break;

		case OPT_NUMBER_THREADS:
			app->ncpu = atoi(optarg);
			break;

		case 'h':
		case OPT_HELP:
			usage(argv[0]);
			return (-1);

		default:
			printf("default; ch=%d\n", ch);
			usage(argv[0]);
			return (-1);
		}
	}
	return (0);
}

/*
 * For now we're using the libevhtp threading model -
 * the FreeBSD-HEAD RSS model would be nicer for what we're
 * doing but it'd be non-portable.
 *
 * Maybe later on I'll abstract out the threading side of
 * things so it can be compile time selected.
 */

int
main(int argc, char ** argv)
{
	struct http_app app;

	if (argc < 2) {
		usage(argv[0]);
		exit(127);
	}

	bzero(&app, sizeof(app));

	app.port = 8080;
	app.ncpu = 1;

	if (parse_opts(&app, argc, argv) < 0)
		exit(127);

	app.evbase = event_base_new();
	app.htp = evhtp_new(app.evbase, NULL);

	signal(SIGPIPE, sighdl_pipe);

	evhtp_set_cb(app.htp, "/line", linecb, NULL);
	evhtp_set_cb(app.htp, "/size", sizecb, NULL);

	evhtp_use_threads(app.htp, http_init_thread, app.ncpu, &app);
	evhtp_bind_socket(app.htp, "0.0.0.0", app.port, 1024);
	event_base_loop(app.evbase, 0);

	exit(0);
}
