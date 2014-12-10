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
#include <sys/queue.h>

#include <netinet/in.h>

#include <evhtp.h>

#include "thr.h"
#include "clt.h"
#include "mgr.h"

//#define	debug_printf(...)
#define	debug_printf(...) fprintf(stderr, __VA_ARGS__)

static int
clt_mgr_conn_start_http_req(struct clt_mgr_conn *c, int msec)
{
	struct timeval tv;

	/* XXX error out if there's a pending request already? */

	c->pending_http_req = 1;

	if (msec <= 0) {
		event_add(c->ev_new_http_req, NULL);
		event_active(c->ev_new_http_req, 0, 0);
	} else {
		tv.tv_sec = msec / 1000;
		tv.tv_usec = (msec % 1000) * 1000;
		event_add(c->ev_new_http_req, &tv);
	}
	return (0);
}

static int
clt_mgr_conn_cancel_http_req(struct clt_mgr_conn *c)
{

	if (c->pending_http_req != 1)
		return (-1);
	event_del(c->ev_new_http_req);
	return (0);
}

static void
clt_mgr_conn_destroy(struct clt_mgr_conn *c)
{

	debug_printf("%s: %p: called; scheduling destroy\n",
	    __func__,
	    c);
	c->is_dead = 1;
	event_add(c->ev_conn_destroy, NULL);
	event_active(c->ev_conn_destroy, 0, 0);
}

static int
clt_mgr_conn_notify_cb(struct client_req *r, clt_notify_cmd_t what,
    void *cbdata)
{
	struct clt_mgr_conn *c = cbdata;

#if 1
	debug_printf("%s: %p: called, r=%p; what=%d (%s)\n",
	    __func__,
	    c,
	    r,
	    what,
	    clt_notify_to_str(what));
#endif

	/* Hack: just keep issuing requests */
	/*
	 * Note: we do this deferred so the rest of the destroy
	 * path in clt.c can run and completely free the request.
	 */
	if (what == CLT_NOTIFY_REQUEST_DONE_OK) {
		c->mgr->req_count_ok++;
	} else if (what == CLT_NOTIFY_REQUEST_DONE_ERROR) {
		c->mgr->req_count_err++;
	} else if (what == CLT_NOTIFY_REQUEST_TIMEOUT) {
		c->mgr->req_count_timeout++;
	} else if (what == CLT_NOTIFY_REQ_DESTROYING) {
		if (c->cur_req_count >= c->target_request_count) {
			/* XXX TODO: close the connection down */
			/* XXX for now; just idle */
			goto finish;
		}
		if (c->is_dead == 1)
			goto finish;
		clt_mgr_conn_start_http_req(c, c->wait_time_pre_http_req_msec);
	} else if (what == CLT_NOTIFY_CONN_CLOSING) {
		/* For now we tear down the owner client too */
		/*
		 * Later on a mgr_conn class instance may re-open
		 * connections, or open multiple clients itself.
		 */
		clt_mgr_conn_cancel_http_req(c);
		clt_mgr_conn_destroy(c);
		return (0);
	}

finish:
	return (0);
}

int
clt_mgr_setup(struct clt_mgr *m)
{
	struct timeval tv;

	/* Start things */
	tv.tv_sec = 0;
	tv.tv_usec = 100000;
	evtimer_add(m->t_timerev, &tv);

	return (0);
}

/*
 * Finish destroying a connection.
 *
 * This must be called via a deferred context so we aren't stuck in
 * the middle of some stack context inside libevhtp/libevent.
 */
static void
_clt_mgr_conn_destroy(struct clt_mgr_conn *c)
{

	/* Delete pending events */
	event_del(c->ev_new_http_req);
	event_del(c->ev_conn_destroy);

	/* Clean up the HTTP request state and connection itself */
	if (c->req)
		clt_conn_destroy(c->req);

	/* Free event */
	event_free(c->ev_new_http_req);
	event_free(c->ev_conn_destroy);

	/* Parent count */
	c->mgr->nconn --;

	/* XXX call back to owner? */

	/* Free connection */
	free(c);
}

static void
clt_mgr_conn_destroy_event(evutil_socket_t sock, short which, void *arg)
{
	struct clt_mgr_conn *c = arg;

	debug_printf("%s: %p: called; destroying\n", __func__, c);

	/*
	 * XXX TODO: I assume that I can delete the event that is calling us
	 * from this context
	 */
	_clt_mgr_conn_destroy(c);
}

static void
clt_mgr_conn_http_req_event(evutil_socket_t sock, short which, void *arg)
{
	struct clt_mgr_conn *c = arg;

	/* XXX TODO: If a HTTP request is pending, warn */
	if (c->req->req != NULL) {
		printf("%s: %p: req in progress?\n", __func__, c);
	}

	/* Issue a new HTTP request */
	c->pending_http_req = 0;
	if (clt_req_create(c->req, c->mgr->uri) < 0) {
		printf("%s: %p: failed to create HTTP connection\n",
		    __func__,
		    c);

		/* XXX TODO should kick off some notification about this? */
		return;
	}

	c->cur_req_count ++;
	c->mgr->req_count++;
}

static struct clt_mgr_conn *
clt_mgr_conn_create(struct clt_mgr *mgr)
{
	struct clt_mgr_conn *c;

	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}
	c->mgr = mgr;
	c->req = clt_conn_create(mgr->thr, clt_mgr_conn_notify_cb,
	    c, mgr->host, mgr->port);
	c->ev_new_http_req = event_new(mgr->thr->t_evbase,
	    -1,
	    0,
	    clt_mgr_conn_http_req_event,
	    c);
	c->ev_conn_destroy = event_new(mgr->thr->t_evbase,
	    -1,
	    0,
	    clt_mgr_conn_destroy_event,
	    c);
	if (c->req == NULL) {
		fprintf(stderr, "%s: clt_conn_create: failed\n", __func__);
		goto error;
	}
	c->target_request_count = mgr->target_request_count;
	c->wait_time_pre_http_req_msec = mgr->wait_time_pre_http_req_msec;

	return (c);

error:
	if (c->req != NULL)
		clt_conn_destroy(c->req);
	if (c->ev_new_http_req != NULL)
		event_free(c->ev_new_http_req);
	if (c->ev_conn_destroy != NULL)
		event_free(c->ev_conn_destroy);
	if (c != NULL)
		free(c);
	return (NULL);
}

static void
clt_mgr_timer(evutil_socket_t sock, short which, void *arg)
{
	int i, j;
	struct clt_mgr *m = arg;
	struct clt_thr *th = m->thr;
	struct timeval tv;
	struct clt_mgr_conn *c;

	tv.tv_sec = 0;
	tv.tv_usec = 100000;

	for (i = m->nconn, j = 0;
	    (i < m->target_nconn &&
	    j <= m->burst_conn);
	    i++, j++) {
		c = clt_mgr_conn_create(m);
		if (c == NULL)
			continue;
		m->nconn ++;
		c->mgr->conn_count++;
		/* Kick start a HTTP request */
		clt_mgr_conn_start_http_req(c, c->wait_time_pre_http_req_msec);
	}

	debug_printf("%s: %p: called\n", __func__, m);
	debug_printf("%s: nconn=%d, conn_count=%llu, req_count=%llu, ok=%llu, err=%llu, timeout=%llu\n",
	    __func__,
	    (int) m->nconn,
	    (unsigned long long) m->conn_count,
	    (unsigned long long) m->req_count,
	    (unsigned long long) m->req_count_ok,
	    (unsigned long long) m->req_count_err,
	    (unsigned long long) m->req_count_timeout);
	evtimer_add(m->t_timerev, &tv);
}

int
clt_mgr_config(struct clt_mgr *m, struct clt_thr *th, const char *host,
    int port, const char *uri)
{
	struct timeval tv;

	m->thr = th;

	/* XXX TODO: error chceking */
	m->host = strdup(host);
	m->port = port;
	m->uri = strdup(uri);

	/* For now, open 8 connections right now */
	/* Later this should be staggered via timer events */
	m->target_nconn = 1;
	m->burst_conn = 128;

	/* Maximum number of requests per connection */
	m->target_request_count = 4;
	m->wait_time_pre_http_req_msec = 1000;

	m->t_timerev = evtimer_new(th->t_evbase, clt_mgr_timer, m);

	return (0);
}

