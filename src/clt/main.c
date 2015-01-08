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
#include "mgr.h"

int
main(int argc, const char *argv[])
{
	struct clt_thr *th;
	struct clt_mgr *m;

	th = calloc(1, sizeof(*th));
	if (th == NULL) {
		err(127, "%s: calloc", __func__);
	}

	/* Create thread state */
	if (clt_thr_setup(th, 0) != 0)
		exit(127);

	m = calloc(1, sizeof(*m));
	if (m == NULL) {
		err(127, "%s: calloc", __func__);
	}

	/* Initial connection setup */
	clt_mgr_setup(m, th);

	/* Test configuration */
	clt_mgr_config(m, "10.11.2.2", 8080, "/size");

	/* Kick things off */
	clt_mgr_start(m);

	/* Begin! */
	event_base_loop(th->t_evbase, 0);

	exit(0);
}
