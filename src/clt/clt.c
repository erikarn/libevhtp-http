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

#include "debug.h"

#include "mgr_stats.h"
#include "thr.h"
#include "clt.h"

const char *
clt_notify_to_str(clt_notify_cmd_t ct)
{
	switch (ct) {
	case CLT_NOTIFY_NONE:
		return "CLT_NOTIFY_NONE";
	case CLT_NOTIFY_CONNECTED:
		return "CLT_NOTIFY_CONNECTED";
	case CLT_NOTIFY_CONNECT_ERROR:
		return "CLT_NOTIFY_CONNECT_ERROR";
	case CLT_NOTIFY_REQUEST_START:
		return "CLT_NOTIFY_REQUEST_START";
	case CLT_NOTIFY_REQUEST_DONE_OK:
		return "CLT_NOTIFY_REQUEST_DONE_OK";
	case CLT_NOTIFY_REQUEST_DONE_ERROR:
		return "CLT_NOTIFY_REQUEST_DONE_ERROR";
	case CLT_NOTIFY_REQUEST_TIMEOUT:
		return "CLT_NOTIFY_REQUEST_TIMEOUT";
	case CLT_NOTIFY_CONN_CLOSING:
		return "CLT_NOTIFY_CONN_CLOSING";
	case CLT_NOTIFY_CONN_DESTROYING:
		return "CLT_NOTIFY_CONN_DESTROYING";
	case CLT_NOTIFY_REQ_DESTROYING:
		return "CLT_NOTIFY_REQ_DESTROYING";
	default:
		return "<unknown>";
	}
}

static void
clt_call_notify(struct client_req *req, clt_notify_cmd_t ct, int data)
{

	if (req->cb.cb != NULL)
		req->cb.cb(req, ct, data, req->cb.cbdata);
}

/*
 * Free a connection, including whichever request is on it.
 *
 * Note: this will call the request free path as well if req is non-NULL.
 */
void
clt_conn_destroy(struct client_req *req)
{

	/* No need to notify the caller; that'll likely be the thing we'd notify */
	//clt_call_notify(req, CLT_NOTIFY_CONN_DESTROYING);

	/*
	 * if there's a request pending on the connection,
	 * then evhtp_connection_free() frees it for us.
	 *
	 * But if we're highly unlucky, we'll free the
	 * request first, but req->con doesn't know
	 * we've done it, and it'll double-free the
	 * request.
	 *
	 * I don't see any code in libevhtp that
	 * clears con->request for us when the request
	 * is freed, leading to what I'm guessing are
	 * some double-free situations.
	 */

	/* free the request; disconnect hooks */
	if (req->req) {
		evhtp_unset_all_hooks(&req->req->hooks);
	}

	/*
	 * So to work around the potential double-free,
	 * assume if we have a con then when we
	 * free it, it'll free the request and
	 * we don't have to.
	 */
	if (req->con) {
		if (req->req != NULL && req->req != req->con->request) {
			printf("%s: different req (%p != %p)\n",
			    __func__,
			    req->req,
			    req->con->request);
			/* it's a different request? Hm */
			evhtp_request_free(req->req);
		}
	} else if (req->req) {
		/* No con, but a request; free it */
		evhtp_request_free(req->req);
	}

	if (req->con)
		evhtp_connection_free(req->con);

	if (req->host_ip)
		free(req->host_ip);
	if (req->host_hdr)
		free(req->host_hdr);
	if (req->uri)
		free(req->uri);

	free(req);
}

/*
 * Destroy the currently active request.
 */
void
clt_req_destroy(struct client_req *req)
{

	debug_printf("%s: %p: called\n", __func__, req);

	clt_call_notify(req, CLT_NOTIFY_REQ_DESTROYING, 0);

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

/*
 * The connection is going away; notify upper layers.
 *
 * This means the evhtp_connection is being freed by libevhtp.
 * Any pending request on the con is also freed.
 */
static evhtp_res
clt_upstream_conn_fini(evhtp_connection_t *conn, void *arg)
{
	struct client_req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);

	/*
	 * XXX for now - assume evhtp is going away; so free any
	 * pending HTTP request and connection.
	 *
	 * XXX and hm, we should really figure out how to /not/
	 * count a submitted HTTP request on this connection, as
	 * it just plainly won't work.
	 */
	evhtp_unset_all_hooks(&conn->hooks);
	r->con = NULL;
	r->req = NULL;

	clt_call_notify(r, CLT_NOTIFY_CONN_CLOSING, 0);

	return (EVHTP_RES_OK);
}

static evhtp_res
clt_upstream_new_chunk(evhtp_request_t * upstream_req, uint64_t len, void * arg)
{
	struct client_req *r = arg;
	struct evbuffer *ei;

	debug_printf("%s: %p: called; len=%lu\n", __func__, r, len);
	debug_printf("%s: %p: req->buffer_in len=%lu\n",
	    __func__,
	    r,
	    evbuffer_get_length(r->req->buffer_in));
	ei = bufferevent_get_input(r->req->conn->bev);
	debug_printf("%s: %p: req->conn->buffer_in len=%lu\n",
	    __func__,
	    r,
	    evbuffer_get_length(ei));
	return EVHTP_RES_OK;
}

evhtp_res
clt_upstream_chunk_done(evhtp_request_t * upstream_req, void * arg)
{
	struct client_req *r = arg;
	size_t len;

	len = evbuffer_get_length(r->req->buffer_in);

	debug_printf("%s: %p: called\n", __func__, r);
	debug_printf("%s: %p: req->buffer_in len=%lu\n",
	    __func__,
	    r,
	    len);

	/* Here's where we consume the incoming data from the input buffer */
	evbuffer_drain(r->req->buffer_in, len);

	return EVHTP_RES_OK;
}

evhtp_res
clt_upstream_chunks_done(evhtp_request_t * upstream_req, void * arg)
{
	struct client_req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);
	/* XXX is this ok here? What if we get 200 OK but we don't get the full response? */
	/* XXX this should be called by the general _cb() request handler */
	clt_call_notify(r, CLT_NOTIFY_REQUEST_DONE_OK, upstream_req->status);

	return (EVHTP_RES_OK);
}

/*
 * Called upon socket error.
 */
static evhtp_res
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

	clt_call_notify(r, CLT_NOTIFY_REQUEST_DONE_ERROR, 0);

	return (EVHTP_RES_OK);
}

static evhtp_res
clt_upstream_headers_start(evhtp_request_t * upstream_req, void *arg)
{
	struct client_req *r = arg;

	/*
	 * XXX TODO: Do the initial book-keeping on response type;
	 * return it upon error/OK
	 */
	debug_printf("%s: %p: status=%d\n", __func__,
	    r, upstream_req->status);
	return (EVHTP_RES_OK);
}

/*
 * Callback: finished - error and non-error
 */
static evhtp_res
clt_upstream_fini(evhtp_request_t * upstream_req, void * arg)
{
	struct client_req *r = arg;

	debug_printf("%s: %p: called\n", __func__, r);

	/* XXX TODO: notify */

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
struct client_req *
clt_conn_create(struct clt_thr *thr, clt_notify_cb *cb, void *cbdata,
    const char *host_ip, const char *host_hdr, int port)
{
	struct client_req *r;

	r = calloc(1, sizeof(*r));
	if (r == NULL) {
		warn("%s: calloc", __func__);
		goto error;
	}

	r->host_ip = strdup(host_ip);
	if (r->host_ip == NULL) {
		warn("%s: strdup\n", __func__);
		goto error;
	}
	r->host_hdr = strdup(host_ip);
	if (r->host_hdr == NULL) {
		warn("%s: strdup\n", __func__);
		goto error;
	}
	r->port = port;
	r->thr = thr;
	r->uri = NULL;	/* No URI yet */
	r->con = evhtp_connection_new(thr->t_evbase, r->host_ip, r->port);
	r->cb.cb = cb;
	r->cb.cbdata = cbdata;
	if (r->con == NULL) {
		debug_printf("%s: thr=%p: evhtp_connection_new failed\n", __func__, thr);
		goto error;
	}

	evhtp_set_hook(&r->con->hooks, evhtp_hook_on_connection_fini,
	    clt_upstream_conn_fini, r);
	debug_printf("%s: %p: called; con=%p\n", __func__, r, r->con);
	return (r);

error:
	if (r && r->host_ip)
		free(r->host_ip);
	if (r && r->host_hdr)
		free(r->host_hdr);
	if (r && r->uri)
		free(r->uri);
	if (r && r->con)
		evhtp_connection_free(r->con);
	if (r != NULL)
		free(r);
	return (NULL);
}

/*
 * Transaction completed - notify upper layers
 *
 * This is called when the transaction is complete,
 * but libevhtp isnt always freeing the underlying transaction
 * for us - there seems to be something in there about
 * freeing on writecb() if it's a keepalive request,
 * but it's not always being freed.
 */
static void
clt_req_cb(evhtp_request_t *r, void *arg)
{
	struct client_req *req = arg;

	debug_printf("%s: %p: called\n", __func__, req);

	/* XXX TODO: hook? */

	evhtp_unset_all_hooks(&req->req->hooks);
//	req->req = NULL;
	req->con->request = NULL;
	clt_req_destroy(req);
}

int
clt_req_create(struct client_req *req, const char *uri, int keepalive)
{

	/* Only do this if there's no outstanding request */
	if (req->req != NULL) {
		fprintf(stderr, "%s: %p: called; req != NULL\n",
		    __func__,
		    req);
		return (-1);
	}

	/* Fail if the connection is closed */
	if (req->con == NULL) {
		fprintf(stderr, "%s: %p: called; conn is NULL\n",
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

	/* Force non-keepalive for now */
	req->is_keepalive = keepalive;

	/* Add headers */
	evhtp_headers_add_header(req->req->headers_out,
	    evhtp_header_new("Host", req->host_hdr, 0, 0));
	evhtp_headers_add_header(req->req->headers_out,
	    evhtp_header_new("User-Agent", "client", 0, 0));

	if (req->is_keepalive)
		evhtp_headers_add_header(req->req->headers_out,
		    evhtp_header_new("Connection", "keep-alive", 0, 0));
	else
		evhtp_headers_add_header(req->req->headers_out,
		    evhtp_header_new("Connection", "close", 0, 0));

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
	evhtp_set_hook(&req->req->hooks, evhtp_hook_on_headers_start,
	    clt_upstream_headers_start, req);

	/* Start request */
	evhtp_make_request(req->con, req->req, htp_method_GET, req->uri);

	debug_printf("%s: %p: done!\n", __func__, req);
	return (0);
}
