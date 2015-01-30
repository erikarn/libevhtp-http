#include <string.h>
#include <stdlib.h>
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
	cfg->target_request_rate = src_cfg->target_request_rate / nthreads;

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

	cfg_ipv4_array_dup(&cfg->ipv4_dst, &src_cfg->ipv4_dst);

	if (src_cfg->host_hdr != NULL)
		cfg->host_hdr = strdup(src_cfg->host_hdr);
	cfg->port = src_cfg->port;
	cfg->uri = strdup(src_cfg->uri);
	cfg->wait_time_pre_http_req_msec = src_cfg->wait_time_pre_http_req_msec;
	cfg->http_keepalive = src_cfg->http_keepalive;

	return (0);
}

void
cfg_ipv4_array_init(struct cfg_ipv4_array *a)
{

	bzero(a, sizeof(*a));
}

int
cfg_ipv4_array_add(struct cfg_ipv4_array *a, const char *addr)
{

	if (a->n >= CFG_IPV4_ARRAY_MAX)
		return (-1);
	a->ipv4[a->n] = strdup(addr);
	a->n++;

	return (0);
}

void
cfg_ipv4_array_dup(struct cfg_ipv4_array *dst, const struct cfg_ipv4_array *src)
{
	int i;

	for (i = 0; i < src->n; i++) {
		dst->ipv4[i] = strdup(src->ipv4[i]);
	}
	dst->n = src->n;
}

const char *
cfg_ipv4_array_get_random(const struct cfg_ipv4_array *r)
{

	/* Don't bother calling random() if we only have one address */
	if (r->n == 1)
		return (r->ipv4[0]);

	return (r->ipv4[random() % r->n]);
}

int
cfg_ipv4_array_nentries(const struct cfg_ipv4_array *r)
{

	return (r->n);
}
