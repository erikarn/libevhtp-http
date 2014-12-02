#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <fcntl.h>
#include <signal.h>

#include <pthread.h>
#include <pthread_np.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <evhtp.h>

//#define	debug_printf(...)
#define	debug_printf(...) fprintf(stderr, __VA_ARGS__)

struct clt_thr {
	int t_tid;		/* thread id; local */
	pthread_t t_thr;
	evbase_t *t_evbase;
	evhtp_t  *t_htp;
	event_t  *t_timerev;

	/*
	 * For now this will be like apachebench - really quite stupid.
	 * Later on we may wish to support some kind of module or
	 * modules that implement a slight smarter testing policy.
	 */

	/* How many open connections */
	int nconn;

	/* how many to attempt to open */
	int target_nconn;
	char *host;
	int port;
	char *uri;
};

/*
 * A client request will have a connection (con) to an IP address, and then
 * one or more outstanding HTTP requests.
 *
 * I'm not sure if libevhtp supports HTTP pipelining at the present time,
 * so let's just assume a single request at a time.
 */
struct client_req {
	evhtp_connection_t *con;
	evhtp_request_t *req;
	struct clt_thr *thr;

	/* Connection details */
	char *host;
	int port;

	/* Request URI */
	char *uri;

	/*
	 * How many requests to issue before this client
	 * request is torn down.
	 */
	int nreq;

	/* How much data was read */
	size_t cur_read_ptr;

#if 0
	/* Read buffer - mostly just scratch-space to read into */
	struct {
		char *buf;
		int size;
	} buf;
#endif
};

/*
 * Free a connection, including whichever request is on it.
 *
 * Note: this will call the request free path as well if req is non-NULL.
 */
static void
clt_conn_destroy(struct client_req *req)
{

	/* free the request; disconnect hooks */
	if (req->req) {
		evhtp_unset_all_hooks(&req->req->hooks);
		evhtp_request_free(req->req);
	}

	if (req->con)
		evhtp_connection_free(req->con);
	if (req->host)
		free(req->host);
	if (req->uri)
		free(req->uri);

	/* XXX remove from connection list */
	req->thr->nconn --;
	free(req);
}

/*
 * Destroy the currently active request.
 */
static void
clt_req_destroy(struct client_req *req)
{


	debug_printf("%s: %p: called\n", __func__, req);

	if (req->req != NULL) {
		/* free the request; disconnect hooks */
		evhtp_unset_all_hooks(&req->req->hooks);
		evhtp_request_free(req->req);
		req->req = NULL;
	}

	/* This is per request */
	if (req->uri)
		free(req->uri);
	req->uri = NULL;
}

static evhtp_res
clt_upstream_new_chunk(evhtp_request_t * upstream_req, uint64_t len, void * arg)
{
	struct client_req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);
	return EVHTP_RES_OK;
}

evhtp_res
clt_upstream_chunk_done(evhtp_request_t * upstream_req, void * arg)
{
	struct client_req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);
	return EVHTP_RES_OK;
}

evhtp_res
clt_upstream_chunks_done(evhtp_request_t * upstream_req, void * arg)
{
	struct client_req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);
	return (EVHTP_RES_OK);
}

/*
 * Called upon socket error.
 */
evhtp_res
clt_upstream_error(evhtp_request_t * req, evhtp_error_flags errtype, void * arg)
{
	struct client_req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);

	/*
	 * We can't destroy the request here;
	 * it seems the bowels of libevent/libevhtp continue
	 * doing stuff with the request.
	 *
	 * clt_upstream_fini() will be called once this request
	 * is freed.
	 */

#if 0
	/* Clear hooks */
//	evhtp_unset_all_hooks(&req->hooks);

	/* Destroy the request */
//	clt_req_destroy(r);

#endif

	return (EVHTP_RES_OK);
}

/*
 * Callback: finished - error and non-error
 */
evhtp_res
clt_upstream_fini(evhtp_request_t * upstream_req, void * arg)
{
	struct client_req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);

	/*
	 * This is called by _evhtp_request_free();
	 * So we don't have to free the request ourselves.
	 */

	evhtp_unset_all_hooks(&r->req->hooks);
	r->req = NULL;

	clt_req_destroy(r);

	return (EVHTP_RES_OK);
}


/*
 * Create a connection to the given host/port.
 *
 * This doesn't create a HTTP request - just the TCP connection.
 */
static struct client_req *
clt_conn_create(struct clt_thr *thr, const char *host, int port)
{
	struct client_req *r;

	r = calloc(1, sizeof(*r));
	if (r == NULL) {
		warn("%s: calloc", __func__);
		goto error;
	}

	r->host = strdup(host);
	if (r->host == NULL) {
		warn("%s: strdup\n", __func__);
		goto error;
	}
	r->port = port;
	r->thr = thr;
	r->uri = NULL;	/* No URI yet */
	r->con = evhtp_connection_new(thr->t_evbase, r->host, r->port);
	if (r->con == NULL) {
		warn("%s: evhtp_connection_new", __func__);
		goto error;
	}

	/* XXX TODO: add to connection list */
	thr->nconn++;

	return (r);

error:
	if (r->host)
		free(r->host);
	if (r->uri)
		free(r->uri);
	if (r->con)
		evhtp_connection_free(r->con);
	return (NULL);
}

/*
 * Transaction completed
 */
static void
clt_req_cb(evhtp_request_t *r, void *arg)
{
	struct client_req *req = arg;

	debug_printf("%s: %p: called\n", __func__, req);

	evhtp_unset_all_hooks(&req->req->hooks);
	req->req = NULL;
	clt_req_destroy(req);
}

static int
clt_req_create(struct client_req *req, const char *uri)
{

	/* Only do this if there's no outstanding request */
	if (req->req != NULL) {
		fprintf(stderr, "%s: %p: called; req != NULL\n",
		    __func__,
		    req);
		return (-1);
	}
	
	req->uri = strdup(uri);
	if (req->uri == NULL) {
		warn("%s: strdup", __func__);
		return (-1);
	}

	req->req = evhtp_request_new(clt_req_cb, req);
	if (req->req == NULL) {
		fprintf(stderr, "%s: %p: failed to create request\n",
		    __func__,
		    req);
		return (-1);
	}

	/* Add headers */
	evhtp_headers_add_header(req->req->headers_out,
	    evhtp_header_new("Host", req->host, 0, 0));
	evhtp_headers_add_header(req->req->headers_out,
	    evhtp_header_new("User-Agent", "client", 0, 0));
	/* XXX how do I mark the actual connection more keep-alive? */
	evhtp_headers_add_header(req->req->headers_out,
	    evhtp_header_new("Connection", "keep-alive", 0, 0));

	/* Hooks */
	evhtp_set_hook(&req->req->hooks, evhtp_hook_on_error,
	    (evhtp_hook) clt_upstream_error, req);
	evhtp_set_hook(&req->req->hooks, evhtp_hook_on_request_fini,
	    clt_upstream_fini, req);
	evhtp_set_hook(&req->req->hooks, evhtp_hook_on_new_chunk,
	    clt_upstream_new_chunk, req);
	evhtp_set_hook(&req->req->hooks, evhtp_hook_on_chunk_complete,
	    clt_upstream_chunk_done, req);
	evhtp_set_hook(&req->req->hooks, evhtp_hook_on_chunks_complete,
	    clt_upstream_chunks_done, req);

	/* Start request */
	evhtp_make_request(req->con, req->req, htp_method_GET, req->uri);

	fprintf(stderr, "%s: %p: done!\n", __func__, req);
	return (0);
}

#if 0
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

	r = req_create(req);
	if (r == NULL) {
		/* XXX need to signal error; close connection */
		evhtp_send_reply(req, EVHTP_RES_ERROR);
		return;
	}
	/* XXX for now, type 'line' */
	req_set_type_buf(r, 131072);
	req_start_response(r);
}
#endif

static int
clt_mgr_setup(struct clt_thr *th)
{
	int i;
	struct client_req *r;

	/* For now, open 8 connections right now */
	/* Later this should be staggered via timer events */
	th->target_nconn = 8;

	for (i = 0; i < 8; i++) {
		r = clt_conn_create(th, th->host, th->port);
		fprintf(stderr, "%s: %p: created\n", __func__, r);
		if (r == NULL)
			continue;
		(void) clt_req_create(r, th->uri);
	}

	return (0);
}

static void
clt_mgr_timer(evutil_socket_t sock, short which, void *arg)
{
	struct clt_thr *th = arg;
	struct timeval tv;

	tv.tv_sec = 1;
	tv.tv_usec = 0;

	debug_printf("%s: %p: called\n", __func__, th);
	evtimer_add(th->t_timerev, &tv);
}

static int
clt_thr_setup(struct clt_thr *th, int tid)
{
	struct timeval tv;

	th->t_tid = tid;
	th->t_evbase = event_base_new();
	th->t_htp = evhtp_new(th->t_evbase, NULL);
	th->t_timerev = evtimer_new(th->t_evbase, clt_mgr_timer, th);

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	evtimer_add(th->t_timerev, &tv);

	th->host = "127.0.0.1";
	th->port = 8080;
	th->uri = "/size";

	return (0);
}

int
main(int argc, const char *argv[])
{
	struct clt_thr *th;

	th = calloc(1, sizeof(*th));
	if (th == NULL) {
		err(127, "%s: calloc", __func__);
	}

	if (clt_thr_setup(th, 0) != 0)
		exit(127);

	/* Create all the client thread state */
	clt_mgr_setup(th);

	/* Begin! */
	event_base_loop(th->t_evbase, 0);

	exit(0);
}
