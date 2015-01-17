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
#include "mgr_config.h"
#include "mgr_stats.h"
#include "thr.h"
#include "mgr.h"

int
clt_thr_setup(struct clt_thr *th, int tid)
{
	struct timeval tv;

	th->t_tid = tid;
	th->t_evbase = event_base_new();
	th->t_htp = evhtp_new(th->t_evbase, NULL);

	pthread_mutex_init(&th->prev_stats_mtx, NULL);

	return (0);
}
