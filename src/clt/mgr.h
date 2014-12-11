#ifndef	__MGR_H__
#define	__MGR_H__

/*
 * This is the instance of a client manager.
 */
struct clt_mgr {
	struct clt_thr *thr;

	/* Periodic event */
	event_t  *t_timerev;

	/* How many open connections */
	int nconn;

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

	/* statistics */
	uint64_t conn_count;

	uint64_t req_count;
	uint64_t req_count_ok;
	uint64_t req_count_err;
	uint64_t req_count_create_err;
	uint64_t req_count_timeout;

	/* Configuration for clients */
	char *host;
	int port;
	char *uri;
	int wait_time_pre_http_req_msec;
	int http_keepalive;
};

/*
 * This is an instance of a client connection, managing
 * an actual client.
 */
struct clt_mgr_conn {
	struct clt_mgr *mgr;

	/* Is this shutting down? */
	int is_dead;

	/* how many requests to make */
	int target_request_count;

	/* how long to wait between each HTTP connection (in msec) */
	int wait_time_pre_http_req_msec;

	/* how many requests have been made thus far */
	int cur_req_count;

	/* The actual connection/request */
	struct client_req *req;

	/* Schedule to issue a new HTTP request */
	event_t *ev_new_http_req;

	/* Shutdown/delete the current connection */
	event_t *ev_conn_destroy;

	/* Is a (new) queued HTTP request pending? */
	/* (Ie, it hasn't yet been started; just queued */
	int pending_http_req;
};

extern	int clt_mgr_config(struct clt_mgr *m, struct clt_thr *th,
	    const char *host, int port, const char *uri);
extern	int clt_mgr_setup(struct clt_mgr *m);

#endif	/* __MGR_H__ */
