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

#include "debug.h"

#include "thr.h"
#include "clt.h"
#include "mgr_stats.h"
#include "mgr_config.h"
#include "mgr.h"


static const char *
clt_mgr_state_str(clt_mgr_state_t state)
{

	switch (state) {
	case CLT_MGR_STATE_NONE:
		return "NONE";
	case CLT_MGR_STATE_INIT:
		return "INIT";
	case CLT_MGR_STATE_RUNNING:
		return "RUNNING";
	case CLT_MGR_STATE_WAITING:
		return "WAITING";
	case CLT_MGR_STATE_CLEANUP:
		return "CLEANUP";
	case CLT_MGR_STATE_CLEANUP_WAITING:
		return "CLEANUP_WAITING";
	case CLT_MGR_STATE_COMPLETED:
		return "COMPLETED";
	default:
		return "<unknown>";
	}
}

static void
clt_mgr_state_change(struct clt_mgr *mgr, clt_mgr_state_t new_state)
{

	printf("%s: changing state from %s to %s\n",
	    __func__,
	    clt_mgr_state_str(mgr->mgr_state),
	    clt_mgr_state_str(new_state));

	/* XXX call callback */

	mgr->mgr_state = new_state;
}

static void
mgr_statustype_update(struct clt_mgr *mgr, int status)
{

	switch (status) {
	case 200:
		mgr->stats.req_statustype_200++;
		break;
	case 302:
		mgr->stats.req_statustype_302++;
		break;
	default:
		mgr->stats.req_statustype_other++;
		break;
	}
}

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

/*
 * Check if the connection is allowed to create another HTTP request.
 *
 * Returns 1 if we are, 0 otherwise.
 */
static int
clt_mgr_conn_check_create_http_request(struct clt_mgr_conn *c)
{
	if (c->is_dead)
		return (0);

	/*
	 * If it's unlimited, don't create a new connection
	 * if we're != RUNNING.
	 */
	if ((c->target_request_count <= 0) &&
	    (c->mgr->mgr_state != CLT_MGR_STATE_RUNNING))
		return(0);

	if ((c->target_request_count > 0) &&
	    (c->cur_req_count >= c->target_request_count))
		return (0);

	return (1);
}

/*
 * Check if the connection manager has hit its limits and we're allowed
 * to create a new HTTP request.
 *
 * Returns 1 if we are, 0 otherwise.
 */
static int
clt_mgr_check_create_http_request(struct clt_mgr *mgr)
{

	if ((mgr->cfg.target_global_request_count > 0) &&
	    mgr->stats.req_count >= mgr->cfg.target_global_request_count)
		return (0);

	return (1);
}

/*
 * Check if the connection manager has hit its limits and we're allowed
 * to create a new connection.
 *
 * Return 1 if we are, 0 otherwise.
 */
static int
clt_mgr_check_create_conn(struct clt_mgr *mgr)
{

	/* Only create connections in INIT/WAITING phases */

	if (mgr->mgr_state != CLT_MGR_STATE_RUNNING &&
	    mgr->mgr_state != CLT_MGR_STATE_INIT)
		return(0);
	if ((mgr->cfg.target_total_nconn_count > 0) &&
	    mgr->stats.conn_count >= mgr->cfg.target_total_nconn_count)
		return (0);
	if (mgr->nconn >= mgr->cfg.target_nconn)
		return (0);

	return (1);
}

/*
 * Check if the connection manager has reached its target run phase
 * and should finish issuing new requests and migrate to WAITING
 * phase (to wait for existing clients to clean up.)
 *
 * This will eventually implement both a timeout based and max
 * connection based check.
 */
static int
clt_mgr_check_finished(struct clt_mgr *mgr)
{

	/* XXX TODO: need a timeout based config option */

	/* number of total requests */
	if ((mgr->cfg.target_global_request_count > 0) &&
	    mgr->stats.req_count >= mgr->cfg.target_global_request_count)
		return (1);
	if ((mgr->cfg.target_total_nconn_count > 0) &&
	    mgr->stats.conn_count >= mgr->cfg.target_total_nconn_count)
		return (1);

	return (0);
}

/*
 * Check if the http clients are completed and no open connections
 * exist.
 */
static int
clt_mgr_check_waiting_finished(struct clt_mgr *mgr)
{

	if (mgr->nconn == 0)
		return (1);
	return (0);
}

static int
clt_mgr_conn_notify_cb(struct client_req *r, clt_notify_cmd_t what,
    int data, void *cbdata)
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
		c->mgr->stats.req_count_ok++;
		mgr_statustype_update(c->mgr, data);
	} else if (what == CLT_NOTIFY_REQUEST_DONE_ERROR) {
		c->mgr->stats.req_count_err++;
	} else if (what == CLT_NOTIFY_REQUEST_TIMEOUT) {
		c->mgr->stats.req_count_timeout++;
	} else if (what == CLT_NOTIFY_REQ_DESTROYING) {
		if (clt_mgr_conn_check_create_http_request(c) &&
		    clt_mgr_check_create_http_request(c->mgr)) {
			clt_mgr_conn_start_http_req(c,
			    c->wait_time_pre_http_req_msec);
		} else {
			clt_mgr_conn_destroy(c);
			/* Close connection */
		}
		return (0);
	} else if (what == CLT_NOTIFY_CONN_CLOSING) {
		/* For now we tear down the owner client too */
		/*
		 * Later on a mgr_conn class instance may re-open
		 * connections, or open multiple clients itself.
		 */
		clt_mgr_conn_cancel_http_req(c);
		clt_mgr_conn_destroy(c);

		/*
		 * XXX TODO:
		 *
		 * We don't know whether it was a failure to connect
		 * or a failure after we've sent the HTTP request;
		 * sigh.
		 */
		c->mgr->stats.conn_closing_count ++;
		return (0);
	}

finish:
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
	/* XXX call back to owner instead? */
	c->mgr->nconn --;
	TAILQ_REMOVE(&c->mgr->mgr_conn_list, c, node);


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
	if (clt_req_create(c->req, c->mgr->cfg.uri, c->mgr->cfg.http_keepalive) < 0) {
		printf("%s: %p: failed to create HTTP connection\n",
		    __func__,
		    c);

		/* XXX TODO should kick off some notification about this? */
		c->mgr->stats.req_count_create_err++;
		return;
	}

	c->cur_req_count++;
	c->mgr->stats.req_count++;
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
	    c, mgr->cfg.host, mgr->cfg.port);
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
	c->target_request_count = mgr->cfg.target_request_count;
	c->wait_time_pre_http_req_msec = mgr->cfg.wait_time_pre_http_req_msec;

	TAILQ_INSERT_TAIL(&mgr->mgr_conn_list, c, node);

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
clt_mgr_waiting_schedule(struct clt_mgr *m)
{
	struct timeval tv;

	tv.tv_sec = m->cfg.waiting_period_sec;
	tv.tv_usec = 0;
	evtimer_add(m->t_wait_timerev, &tv);
}

static void
clt_mgr_waiting_deschedule(struct clt_mgr *m)
{

	evtimer_del(m->t_wait_timerev);
}

static void
clt_mgr_cleanup_schedule(struct clt_mgr *m)
{
	struct timeval tv;

	/* Short wait for cleanup */
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	evtimer_add(m->t_cleanup_timerev, &tv);
}

static void
clt_mgr_cleanup_deschedule(struct clt_mgr *m)
{

	evtimer_del(m->t_cleanup_timerev);
}

static void
clt_mgr_state_set_waiting(struct clt_mgr *m)
{

	clt_mgr_state_change(m, CLT_MGR_STATE_WAITING);
	clt_mgr_waiting_schedule(m);
	evtimer_del(m->t_running_timerev);
}

static void
clt_mgr_set_running_timer(struct clt_mgr *m)
{
	struct timeval tv;
	if (m->cfg.running_period_sec < 0)
		return;

	tv.tv_sec = m->cfg.running_period_sec;
	tv.tv_usec = 0;
	evtimer_add(m->t_running_timerev, &tv);

}

static void
clt_mgr_timer_state_running(struct clt_mgr *m)
{
	int i, j;
	struct clt_thr *th = m->thr;
	struct clt_mgr_conn *c;

	for (i = m->nconn, j = 0;
	    (i < m->cfg.target_nconn &&
	    j <= m->cfg.burst_conn);
	    i++, j++) {
		/* break if we hit our global connection limit */
		if (! clt_mgr_check_create_conn(m))
			break;
		c = clt_mgr_conn_create(m);
		if (c == NULL)
			continue;
		m->nconn ++;
		c->mgr->stats.conn_count++;
		/* Kick start a HTTP request */
		clt_mgr_conn_start_http_req(c, c->wait_time_pre_http_req_msec);
	}

	/*
	 * If we hit our limit then transition to
	 * WAITING - we'll wait until there are no more
	 * connections active, then we'll cleanup.
	 */
	if (clt_mgr_check_finished(m)) {
		clt_mgr_state_set_waiting(m);
	}
}

static void
clt_mgr_waiting_timer(evutil_socket_t sock, short which, void *arg)
{
	struct clt_mgr *m = arg;

	printf("%s: called\n", __func__);
	if (m->mgr_state != CLT_MGR_STATE_WAITING) {
		printf("%s: unexpected state? (%s)\n",
		    __func__,
		    clt_mgr_state_str(m->mgr_state));
		return;
	}

	/* Bump to CLEANUP phase */
	clt_mgr_state_change(m, CLT_MGR_STATE_CLEANUP);
	clt_mgr_cleanup_schedule(m);
}

static void
clt_mgr_timer_state_waiting(struct clt_mgr *m)
{

	if (! clt_mgr_check_waiting_finished(m))
		return;

	if (m->mgr_state != CLT_MGR_STATE_WAITING) {
		printf("%s: unexpected state? (%s)\n",
		    __func__,
		    clt_mgr_state_str(m->mgr_state));
		return;
	}

	/* Bump to CLEANUP phase */
	clt_mgr_state_change(m, CLT_MGR_STATE_CLEANUP);
	clt_mgr_waiting_deschedule(m);
	clt_mgr_cleanup_schedule(m);
}

static void
clt_mgr_timer_state_cleanup_waiting(struct clt_mgr *m)
{
	if (m->mgr_state != CLT_MGR_STATE_CLEANUP_WAITING) {
		printf("%s: unexpected state? (%s)\n",
		    __func__,
		    clt_mgr_state_str(m->mgr_state));
		return;
	}

	/* Are all the connections closed and destroyed? */
	/* XXX methodize this! */
	if (m->nconn != 0)
		return;

	/* Deleting the final timer causes the event loop to exit */
	/* We shouldn't rely on this specific behaviour though! */
	clt_mgr_state_change(m, CLT_MGR_STATE_COMPLETED);
	evtimer_del(m->t_timerev);
}

static void
clt_mgr_cleanup_timer(evutil_socket_t sock, short which, void *arg)
{
	struct clt_mgr *m = arg;
	struct clt_mgr_conn *c, *cn;

	printf("%s: called\n", __func__);

	if (m->mgr_state != CLT_MGR_STATE_CLEANUP) {
		printf("%s: unexpected state? (%s)\n",
		    __func__,
		    clt_mgr_state_str(m->mgr_state));
		return;
	}

	/* Walk the list of open http connections, forcibly closing them */
	TAILQ_FOREACH_SAFE(c, &m->mgr_conn_list, node, cn) {
		clt_mgr_conn_destroy(c);
	}

	/* Bump to COMPLETED_WAITING phase to cleanup */
	clt_mgr_state_change(m, CLT_MGR_STATE_CLEANUP_WAITING);
}

static void
clt_mgr_running_timer(evutil_socket_t sock, short which, void *arg)
{
	struct clt_mgr *m = arg;

	/* Timer has fired; so time to force a state to WAITING */
	clt_mgr_state_set_waiting(m);
}

static void
clt_mgr_stat_timer(evutil_socket_t sock, short which, void *arg)
{
	struct clt_mgr *m = arg;
	struct timeval tv;

	printf("%s: (%d): %s: nconn=%d, conn_count=%llu, conn closing=%llu, req_count=%llu, ok=%llu, err=%llu, timeout=%llu\n",
	    __func__,
	    m->thr->t_tid,
	    clt_mgr_state_str(m->mgr_state),
	    (int) m->nconn,
	    (unsigned long long) m->stats.conn_count,
	    (unsigned long long) m->stats.conn_closing_count,
	    (unsigned long long) m->stats.req_count,
	    (unsigned long long) m->stats.req_count_ok,
	    (unsigned long long) m->stats.req_count_err,
	    (unsigned long long) m->stats.req_count_timeout);
	printf("%s: 200_OK: %llu, 302: %llu, Other: %llu\n",
	    __func__,
	    (unsigned long long) m->stats.req_statustype_200,
	    (unsigned long long) m->stats.req_statustype_302,
	    (unsigned long long) m->stats.req_statustype_other);

	/* Don't add the timer again if we've hit COMPLETED */
	if (m->mgr_state == CLT_MGR_STATE_COMPLETED)
		return;

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	evtimer_add(m->t_stat_timerev, &tv);
}

static void
clt_mgr_timer(evutil_socket_t sock, short which, void *arg)
{
	int i, j;
	struct clt_mgr *m = arg;
	struct timeval tv;

	switch (m->mgr_state) {
	case CLT_MGR_STATE_RUNNING:
		clt_mgr_timer_state_running(m);
		break;
	case CLT_MGR_STATE_WAITING:
		clt_mgr_timer_state_waiting(m);
		break;
	case CLT_MGR_STATE_CLEANUP:
		break;
	case CLT_MGR_STATE_CLEANUP_WAITING:
		clt_mgr_timer_state_cleanup_waiting(m);
		break;
	case CLT_MGR_STATE_COMPLETED:
		break;
	default:
		/* XXX this shouldn't happen! */
		printf("%s: not in a valid state yet (%s)\n",
		    __func__,
		    clt_mgr_state_str(m->mgr_state));
	}

	/* Nope, don't add the timer again if we've hit COMPLETED */
	if (m->mgr_state == CLT_MGR_STATE_COMPLETED)
		return;

	tv.tv_sec = 0;
	tv.tv_usec = 100000;
	evtimer_add(m->t_timerev, &tv);
}

int
clt_mgr_setup(struct clt_mgr *m, struct clt_thr *th)
{
	m->thr = th;

	/* Tracking list! */
	TAILQ_INIT(&m->mgr_conn_list);

	m->t_timerev = evtimer_new(th->t_evbase, clt_mgr_timer, m);
	m->t_stat_timerev = evtimer_new(th->t_evbase, clt_mgr_stat_timer, m);
	m->t_wait_timerev = evtimer_new(th->t_evbase, clt_mgr_waiting_timer, m);
	m->t_cleanup_timerev = evtimer_new(th->t_evbase, clt_mgr_cleanup_timer, m);
	m->t_running_timerev = evtimer_new(th->t_evbase, clt_mgr_running_timer, m);

	return (0);
}

int
clt_mgr_start(struct clt_mgr *m)
{
	struct timeval tv;

	/* Set running timer */
	clt_mgr_set_running_timer(m);

	/* Bump to running */
	clt_mgr_state_change(m, CLT_MGR_STATE_RUNNING);

	/* Start things */
	tv.tv_sec = 0;
	tv.tv_usec = 100000;
	evtimer_add(m->t_timerev, &tv);

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	evtimer_add(m->t_stat_timerev, &tv);

	return (0);
}


