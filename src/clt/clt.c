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

	return (0);
}

static int
clt_mgr_config(struct clt_thr *th, const char *host, int port, const char *uri)
{

	/* XXX TODO: error chceking */
	th->host = strdup(host);
	th->port = 8080;
	th->uri = strdup(uri);

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

	/* Create thread state */
	if (clt_thr_setup(th, 0) != 0)
		exit(127);

	/* Test configuration */
	clt_mgr_config(th, "127.0.0.1", 8080, "/size");

	/* Initial connection setup */
	clt_mgr_setup(th);

	/* Begin! */
	event_base_loop(th->t_evbase, 0);

	exit(0);
}
