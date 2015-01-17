#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <fcntl.h>
#include <signal.h>

#include <pthread.h>
#include <pthread_np.h>

#include <getopt.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <evhtp.h>

#include "debug.h"
#include "thr.h"
#include "clt.h"
#include "mgr_stats.h"
#include "mgr_config.h"
#include "mgr.h"

void
sighdl_pipe(int s)
{
}

static void
mgr_config_defaults(struct mgr_config *cfg)
{
	/* XXX TODO: error checking */
	cfg->host = NULL;
	cfg->port = -1;
	cfg->uri = NULL;

	/* Default number of threads */
	cfg->num_threads = 1;

	/* How many connections to keep open */
	cfg->target_nconn = 128;

	/* How many to try and open every 100ms */
	cfg->burst_conn = 10;

	/* Maximum number of requests per connection; -1 for unlimited */
	cfg->target_request_count = -1;

	/* Time to wait (msec) before issuing a HTTP request */
	cfg->wait_time_pre_http_req_msec = 1;

	/* How many global connections to make, -1 for no limit */
	cfg->target_total_nconn_count = -1;

	/* How many global requests to make, -1 for no limit */
	cfg->target_global_request_count = -1;

	/* Keepalive? (global for now) */
	cfg->http_keepalive = 1;

	/*
	 * How long to run the test for in RUNNING, before
	 * we transition to WAITING regardless, or -1 for
	 * no time based limit.
	 */
	cfg->running_period_sec = 30;

	/*
	 * How long to wait around during WAITING for connections
	 * to finish and close
	 */
	cfg->waiting_period_sec = 30;
}

enum {
	OPT_HOST_IP = 1000,
	OPT_PORT,
	OPT_URI,
	OPT_TARGET_NCONN,
	OPT_BURST_CONN,
	OPT_TARGET_REQUEST_COUNT,
	OPT_WAIT_TIME_PRE_HTTP_REQ_MSEC,
	OPT_TARGET_TOTAL_NCONN_COUNT,
	OPT_TARGET_GLOBAL_REQUEST_COUNT,
	OPT_HTTP_KEEPALIVE,
	OPT_RUNNING_PERIOD,
	OPT_WAITING_PERIOD,
	OPT_NUMBER_THREADS,
};

static struct option longopts[] = {
	{ "host-ip", required_argument, NULL, OPT_HOST_IP },
	{ "port", required_argument, NULL, OPT_PORT },
	{ "uri", required_argument, NULL, OPT_URI },
	{ "target-nconn", required_argument, NULL, OPT_TARGET_NCONN },
	{ "burst-conn", required_argument, NULL, OPT_BURST_CONN },
	{ "target-request-count", required_argument, NULL, OPT_TARGET_REQUEST_COUNT },
	{ "wait-time-request-msec", required_argument, NULL, OPT_WAIT_TIME_PRE_HTTP_REQ_MSEC },
	{ "target-total-nconn-count", required_argument, NULL, OPT_TARGET_TOTAL_NCONN_COUNT } ,
	{ "target-global-request-count", required_argument, NULL, OPT_TARGET_GLOBAL_REQUEST_COUNT },
	{ "http-keepalive", required_argument, NULL, OPT_HTTP_KEEPALIVE },
	{ "running-period", required_argument, NULL, OPT_RUNNING_PERIOD },
	{ "waiting-period", required_argument, NULL, OPT_WAITING_PERIOD },
	{ "number-threads", required_argument, NULL, OPT_NUMBER_THREADS },
	{ "help", no_argument, NULL, 'h' },
	{ NULL, 0, NULL, 0 },
};

static void
usage(char *progname)
{
	printf("Usage:\n");
	printf("  %s --host-ip=<host ipv4> --port=<port> --uri=<uri>\n", progname);
	printf("\n");
	printf("  Required options:\n");
	printf("    host-ip: ipv4 address\n");
	printf("    port: HTTP port\n");
	printf("    uri: URI path (eg /size)\n");
	printf("\n");
	printf("  Optional options:\n");
	printf("    --number-threads=<number of worker threads>\n");
	printf("    --target-nconn=<target number of concurrent connections>\n");
	printf("    --burst-conn=<how many connections to open every 100ms>\n");
	printf("    --target-request-count=<request count per connection, or -1 for unlimited>\n");
	printf("    --wait-time-request-msec=<wait time before each request, in msec>\n");
	printf("    --target-total-nconn-count=<total number of connections, or -1 for unlimited>\n");
	printf("    --target-global-request-count=<total number of requests, or -1 for unlimited>\n");
	printf("    --http-keepalive=<1 to enable keepalive, 0 for none>\n");
	printf("    --running-period=<how long to run in seconds, or -1 for no time period>\n");
	printf("    --waiting-period=<how long to wait to cleanup in seconds>\n");
	printf("    --help - this help\n");

	return;
}

static int
parse_opts(struct mgr_config *cfg, int argc, char *argv[])
{
	int ch;

	while ((ch = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
			usage(argv[0]);
			return (-1);

		case OPT_HOST_IP:
			if (cfg->host != NULL)
				free(cfg->host);
			cfg->host = strdup(optarg);
			break;

		case OPT_PORT:
			cfg->port = atoi(optarg);
			break;

		case OPT_URI:
			if (cfg->uri != NULL)
				free(cfg->uri);
			cfg->uri = strdup(optarg);
			break;

		case OPT_TARGET_NCONN:
			cfg->target_nconn = atoi(optarg);
			break;

		case OPT_BURST_CONN:
			cfg->burst_conn = atoi(optarg);
			break;

		case OPT_TARGET_REQUEST_COUNT:
			cfg->target_request_count = atoi(optarg);
			break;

		case OPT_WAIT_TIME_PRE_HTTP_REQ_MSEC:
			cfg->wait_time_pre_http_req_msec = atoi(optarg);
			break;

		case OPT_TARGET_TOTAL_NCONN_COUNT:
			cfg->target_total_nconn_count = atoi(optarg);
			break;

		case OPT_HTTP_KEEPALIVE:
			cfg->http_keepalive = atoi(optarg);
			break;

		case OPT_RUNNING_PERIOD:
			cfg->running_period_sec = atoi(optarg);
			break;

		case OPT_WAITING_PERIOD:
			cfg->waiting_period_sec = atoi(optarg);
			break;

		case OPT_NUMBER_THREADS:
			cfg->num_threads = atoi(optarg);
			break;

		default:
			usage(argv[0]);
			return (-1);
		}
	}
	return (0);
}

void *
clt_mgr_thread_run(void *arg)
{
	struct clt_thr *th = arg;
	char buf[32];

	printf("[%d] th=%p\n", th->t_tid, th);
	snprintf(buf, 128, "(%d)", th->t_tid);
	(void) pthread_set_name_np(th->t_thr, buf);

	/* Kick things off */
	clt_mgr_start(th->t_m);

	/* Begin! */
	event_base_loop(th->t_evbase, 0);

	return (NULL);
}

int
main(int argc, char *argv[])
{
	struct clt_thr *th;
	struct mgr_config cfg;
	int i;

	/* Parse configuration early */
	bzero(&cfg, sizeof(cfg));

	/* Defaults */
	mgr_config_defaults(&cfg);

	/* Parse */
	if (parse_opts(&cfg, argc, argv) != 0)
		exit(128);

	/* Minimum config: host, port, ip */
	if (cfg.host == NULL || cfg.uri == NULL || cfg.port == -1) {
		usage(argv[0]);
		exit(128);
	}

	signal(SIGPIPE, sighdl_pipe);

	evthread_use_pthreads();

	/* Allocate worker thread state */
	th = calloc(cfg.num_threads, sizeof(struct clt_thr));
	if (th == NULL) {
		err(127, "%s: calloc", __func__);
	}

	/* Setup initial workers with local configuration */
	for (i = 0; i < cfg.num_threads; i++) {
		if (clt_thr_setup(&th[i], i) != 0)
			exit(127);
		th[i].t_m = calloc(1, sizeof(struct clt_mgr));
		if (th[i].t_m == NULL)
			err(127, "%s: calloc", __func__);

		/* Setup each client */
		clt_mgr_setup(th[i].t_m, &th[i]);
	}

	/*
	 * Ok, for now let's cheap out and just allocate 1/i
	 * of the various configuration parameters to each.
	 * worker thread.
	 *
	 * Why's this a bad idea? If one CPU falls a bit behind,
	 * then the target request rate may not really be
	 * exactly what we're after.  Ie, we can't shift
	 * load between worker threads if we need to.
	 *
	 * But, ENOTIME, etc.
	 */
	for (i = 0; i < cfg.num_threads; i++) {
		mgr_config_copy_thread(&cfg, &th[i].t_m->cfg, cfg.num_threads);
	}

	/* Now, start each thread */
	for (i = 0; i < cfg.num_threads; i++) {
		if (pthread_create(&th[i].t_thr, NULL, clt_mgr_thread_run, &th[i]) != 0)
			err(127, "%s: pthread_create", __func__);
	}

	/* Now, join */
	for (i = 0; i < cfg.num_threads; i++) {
		(void) pthread_join(th[i].t_thr, NULL);
	}

	exit(0);
}
