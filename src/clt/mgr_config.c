#include <string.h>
#include "mgr_config.h"

/*
 * Divvy up the thread contents of the configuration
 * setup to each thread.
 */
int
mgr_config_copy_thread(const struct mgr_config *src_cfg,
    struct mgr_config *cfg, int nthreads)
{

	/* Paranoia */
	if (nthreads == 0)
		nthreads = 1;

	cfg->num_threads = src_cfg->num_threads;
	cfg->burst_conn = src_cfg->burst_conn / nthreads;
	cfg->target_nconn = src_cfg->target_nconn / nthreads;
	cfg->target_request_count = src_cfg->target_request_count;

	if (src_cfg->target_global_request_count > 0) {
		cfg->target_global_request_count = src_cfg->target_global_request_count / nthreads;
	} else {
		cfg->target_global_request_count = src_cfg->target_global_request_count;
	}

	if (src_cfg->target_total_nconn_count > 0) {
		cfg->target_total_nconn_count = src_cfg->target_total_nconn_count / nthreads;
	} else {
		cfg->target_total_nconn_count = src_cfg->target_total_nconn_count;
	}

	cfg->running_period_sec = src_cfg->running_period_sec;
	cfg->waiting_period_sec = src_cfg->waiting_period_sec;

	cfg->host = strdup(src_cfg->host);
	cfg->port = src_cfg->port;
	cfg->uri = strdup(src_cfg->uri);
	cfg->wait_time_pre_http_req_msec = src_cfg->wait_time_pre_http_req_msec;
	cfg->http_keepalive = src_cfg->http_keepalive;

	return (0);
}
