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

#include "clt.h"

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
 * Free a connection, including whichever request is on it.
 *
 * Note: this will call the request free path as well if req is non-NULL.
 */
void
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
void
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
static evhtp_res
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
struct client_req *
clt_conn_create(struct clt_thr *thr, clt_notify_cb *cb, void *cbdata,
    const char *host, int port)
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
	r->cb.cb = cb;
	r->cb.cbdata = cbdata;
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

int
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