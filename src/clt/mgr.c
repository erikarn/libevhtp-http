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

int
clt_mgr_setup(struct clt_mgr *m)
{
	int i;
	struct client_req *r;
	struct clt_thr *th = m->thr;

	for (i = 0; i < m->target_nconn; i++) {
		r = clt_conn_create(th, m->host, m->port);
		fprintf(stderr, "%s: %p: created\n", __func__, r);
		if (r == NULL)
			continue;

		/* XXX there's no link back to the mgr yet, grr. */
		(void) clt_req_create(r, m->uri);
	}

	return (0);
}

static void
clt_mgr_timer(evutil_socket_t sock, short which, void *arg)
{
	struct clt_mgr *m = arg;
	struct timeval tv;

	tv.tv_sec = 1;
	tv.tv_usec = 0;

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

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	m->t_timerev = evtimer_new(th->t_evbase, clt_mgr_timer, m);
	evtimer_add(m->t_timerev, &tv);

	return (0);
}

