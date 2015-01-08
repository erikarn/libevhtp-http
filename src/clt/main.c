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
#include "thr.h"
#include "clt.h"
#include "mgr_config.h"
#include "mgr.h"

void
sighdl_pipe(int s)
{
}

static void
mgr_config(struct mgr_config *cfg, const char *host, int port, const char *uri)
{
	/* XXX TODO: error checking */
	cfg->host = strdup(host);
	cfg->port = port;
	cfg->uri = strdup(uri);

	/* How many connections to keep open */
	cfg->target_nconn = 16384;

	/* How many to try and open every 100ms */
	cfg->burst_conn = 1024;

	/* Maximum number of requests per connection; -1 for unlimited */
	cfg->target_request_count = -1;

	/* Time to wait (msec) before issuing a HTTP request */
	cfg->wait_time_pre_http_req_msec = 1;

	/* How many global connections to make, -1 for no limit */
	cfg->target_total_nconn_count = -1;

	/* How many global requests to make, -1 for no limit */
	cfg->target_global_request_count = 20480;

	/* Keepalive? (global for now) */
	cfg->http_keepalive = 1;

	/*
	 * How long to run the test for in RUNNING, before
	 * we transition to WAITING regardless, or -1 for
	 * no time based limit.
	 */
	cfg->running_period_sec = 3;

	/*
	 * How long to wait around during WAITING for connections
	 * to finish and close
	 */
	cfg->waiting_period_sec = 10;
}

int
main(int argc, const char *argv[])
{
	struct clt_thr *th;
	struct clt_mgr *m;

	th = calloc(1, sizeof(*th));
	if (th == NULL) {
		err(127, "%s: calloc", __func__);
	}

	signal(SIGPIPE, sighdl_pipe);

	/* Create thread state */
	if (clt_thr_setup(th, 0) != 0)
		exit(127);

	m = calloc(1, sizeof(*m));
	if (m == NULL) {
		err(127, "%s: calloc", __func__);
	}

	/* Initial connection setup */
	clt_mgr_setup(m, th);

	/* Setup test configuration */
	mgr_config(&m->cfg, "10.11.2.1", 8080, "/size");

	/* Kick things off */
	clt_mgr_start(m);

	/* Begin! */
	event_base_loop(th->t_evbase, 0);

	exit(0);
}
