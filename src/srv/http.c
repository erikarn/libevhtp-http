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

#define	debug_printf(...)
//#define	debug_printf(...) fprintf(stderr, __VA_ARGS__)

typedef enum {
	REQ_TYPE_NONE,
	REQ_TYPE_LINE,
	REQ_TYPE_SIZE,
} req_type_t;

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

int
main(int argc, char ** argv) {
    evbase_t * evbase = event_base_new();
    evhtp_t  * htp    = evhtp_new(evbase, NULL);

    evhtp_set_cb(htp, "/line", linecb, NULL);
    evhtp_set_cb(htp, "/size", sizecb, NULL);
    evhtp_bind_socket(htp, "0.0.0.0", 8080, 1024);
    event_base_loop(evbase, 0);
    return 0;
}
