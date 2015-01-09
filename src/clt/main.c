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

	/* How many connections to keep open */
	cfg->target_nconn = 128;

	/* How many to try and open every 100ms */
	cfg->burst_conn = 10;

	/* Maximum number of requests per connection; -1 for unlimited */
	cfg->target_request_count = 10;

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
	OPT_WAITING_PERIOD
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

		default:
			usage(argv[0]);
			return (-1);
		}
	}
	return (0);
}

int
main(int argc, char *argv[])
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

	/* Default configuration */
	mgr_config_defaults(&m->cfg);

	/* Parse */
	if (parse_opts(&m->cfg, argc, argv) != 0)
		exit(128);

	/* Minimum config: host, port, ip */
	if (m->cfg.host == NULL || m->cfg.uri == NULL || m->cfg.port == -1) {
		usage(argv[0]);
		exit(128);
	}

	/* Kick things off */
	clt_mgr_start(m);

	/* Begin! */
	event_base_loop(th->t_evbase, 0);

	exit(0);
}
