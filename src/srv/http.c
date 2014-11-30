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

struct req {
	int cur_count;
	int max_count;
	evhtp_request_t *req;
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

	r->max_count = 16;
	r->cur_count = 0;
	r->req = req;

	return r;
}

static void
req_free(struct req *r)
{

	/* XXX what about req? */
	free(r);
}

static evhtp_res
send_upstream_new_chunk(evhtp_request_t * upstream_req, uint64_t len, void * arg)
{

	debug_printf("%s: called\n", __func__);
	return EVHTP_RES_OK;
}

evhtp_res
send_upstream_chunk_done(evhtp_request_t * upstream_req, void * arg)
{

	debug_printf("%s: called\n", __func__);
	return EVHTP_RES_OK;
}

evhtp_res
send_upstream_chunks_done(evhtp_request_t * upstream_req, void * arg)
{

	debug_printf("%s: called\n", __func__);
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

	req_free(r);

	return (EVHTP_RES_OK);
}

evhtp_res
send_upstream_on_write(evhtp_connection_t * conn, void * arg)
{
	struct req *r = arg;
	struct evbuffer *evb;

	debug_printf("%s: called\n", __func__);

	evb = evbuffer_new();
	/* XXX free callback - not needed; this is a static buffer */
	evbuffer_add_reference(evb, "foobar\r\n", 8, NULL, NULL);
	evhtp_send_reply_chunk(r->req, evb);
	evbuffer_free(evb);

#if 1
	r->cur_count++;
	if (r->cur_count >= r->max_count) {
		evhtp_unset_hook(&r->req->conn->hooks, evhtp_hook_on_write);
		evhtp_send_reply_chunk_end(r->req);
		/* This will wrap up the connection for us via the fini path */
	}
#endif

	return (EVHTP_RES_OK);
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
	evhtp_send_reply_chunk_start(r->req, EVHTP_RES_OK);

	/* Send the initial buffer */
	evb = evbuffer_new();
	/* XXX free callback - not needed; this is a static buffer */
	evbuffer_add_reference(evb, "foobar\r\n", 8, NULL, NULL);
	evhtp_send_reply_chunk(r->req, evb);
	evbuffer_free(evb);
}

void
testcb(evhtp_request_t * req, void * a)
{
	struct req *r;

	r = req_create(req);
	if (r == NULL) {
		/* XXX need to signal error; close connection */
		evhtp_send_reply(req, EVHTP_RES_ERROR);
		return;
	}

	req_start_response(r);
}

int
main(int argc, char ** argv) {
    evbase_t * evbase = event_base_new();
    evhtp_t  * htp    = evhtp_new(evbase, NULL);

    evhtp_set_cb(htp, "/test", testcb, NULL);
    evhtp_bind_socket(htp, "0.0.0.0", 8080, 1024);
    event_base_loop(evbase, 0);
    return 0;
}
