#ifndef	__MGR_CONFIG_H__
#define	__MGR_CONFIG_H__

/*
 * This is the instance of a client manager.
 */
struct mgr_config {

	/* How many client worker threads */
	int num_threads;

	/* How many per burst */
	int burst_conn;

	/* how many to attempt to open */
	int target_nconn;

	/* how many requests each conn should run before finishing */
	int target_request_count;

	/* .. and a global limit */
	int target_global_request_count;

	/* how many connections should be run before finishing */
	int target_total_nconn_count;

	/* How long to run for in RUNNING before finishing */
	int running_period_sec;

	/* WAITING phase configuration */
	/*
	 * How long to wait for connections to complete
	 * before moving to CLEANUP.
	 */
	int waiting_period_sec;

	/* Configuration for clients */
	char *host;
	int port;
	char *uri;
	int wait_time_pre_http_req_msec;
	int http_keepalive;
};

extern	int mgr_config_copy_thread(const struct mgr_config *src_cfg,
	    struct mgr_config *cfg, int nthreads);

#endif	/* __MGR_CONFIG_H__ */
