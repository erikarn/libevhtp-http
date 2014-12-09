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

#include "thr.h"
#include "clt.h"
#include "mgr.h"

//#define	debug_printf(...)
#define	debug_printf(...) fprintf(stderr, __VA_ARGS__)

static int
clt_mgr_conn_notify_cb(struct client_req *r, clt_notify_cmd_t what,
    void *cbdata)
{
	struct clt_mgr_conn *c = cbdata;

	debug_printf("%s: %p: called, r=%p; what=%d (%s)\n",
	    __func__,
	    c,
	    r,
	    what,
	    clt_notify_to_str(what));

	return (0);
}

int
clt_mgr_setup(struct clt_mgr *m)
{
	struct timeval tv;

	/* Start things */
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	evtimer_add(m->t_timerev, &tv);

	return (0);
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
	if (c->req == NULL) {
		fprintf(stderr, "%s: clt_conn_create: failed\n", __func__);
		goto error;
	}

	/* Kick start a HTTP request */
	(void) clt_req_create(c->req, mgr->uri);

	return (c);

error:
	if (c->req != NULL)
		clt_conn_destroy(c->req);
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

	tv.tv_sec = 1;
	tv.tv_usec = 0;

	for (i = m->nconn, j = 0;
	    (i < m->target_nconn &&
	    j <= m->burst_conn);
	    i++, j++) {
		c = clt_mgr_conn_create(m);
		if (c == NULL)
			continue;
		m->nconn ++;
	}

	debug_printf("%s: %p: called\n", __func__, m);
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
	m->target_nconn = 8;
	m->burst_conn = 4;

	m->t_timerev = evtimer_new(th->t_evbase, clt_mgr_timer, m);

	return (0);
}

