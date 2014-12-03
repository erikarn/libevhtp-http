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
	struct clt_mgr *m = cbdata;

	debug_printf("%s: %p: called, r=%p; what=%d\n",
	    __func__,
	    m,
	    r,
	    what);

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

static void
clt_mgr_timer(evutil_socket_t sock, short which, void *arg)
{
	int i, j;
	struct clt_mgr *m = arg;
	struct clt_thr *th = m->thr;
	struct timeval tv;
	struct client_req *r;

	tv.tv_sec = 1;
	tv.tv_usec = 0;

	for (i = m->nconn, j = 0;
	    (i < m->target_nconn &&
	    j <= m->burst_conn);
	    i++, j++) {
		/*
		 * For now the global manager will just look after
		 * each connection; I'll worry about allocating
		 * per-connection (and maybe later per-request)
		 * mgr state later.
		 */
		r = clt_conn_create(th, clt_mgr_conn_notify_cb, m,
		    m->host, m->port);

		/* Keep track of how many connections we have open */
		m->nconn++;

		fprintf(stderr, "%s: %p: created\n", __func__, r);

		if (r == NULL)
			continue;

		/* Kick start a HTTP request */
		(void) clt_req_create(r, m->uri);
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

