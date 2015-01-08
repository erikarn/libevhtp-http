#ifndef	__MGR_H__
#define	__MGR_H__

typedef enum {
	CLT_MGR_STATE_NONE,
	CLT_MGR_STATE_INIT,
	CLT_MGR_STATE_RUNNING,
	CLT_MGR_STATE_WAITING,
	CLT_MGR_STATE_CLEANUP,
	CLT_MGR_STATE_CLEANUP_WAITING,
	CLT_MGR_STATE_COMPLETED
} clt_mgr_state_t;

struct clt_mgr_conn;

/*
 * This is the instance of a client manager.
 */
struct clt_mgr {
	struct clt_thr *thr;

	/* Current state */
	clt_mgr_state_t mgr_state;

	/* List of http connection s*/
	TAILQ_HEAD(, clt_mgr_conn) mgr_conn_list;

	/* Periodic event */
	event_t  *t_timerev;

	/* RUNNING timer event */
	event_t *t_running_timerev;

	/* WAITING timer event */
	event_t *t_wait_timerev;

	/* CLEANUP timer event */
	event_t *t_cleanup_timerev;

	/* Configuration */
	struct mgr_config cfg;

	/* How many open connections */
	int nconn;

	/* statistics */
	uint64_t conn_count;
	uint64_t conn_closing_count;

	uint64_t req_count;
	uint64_t req_count_ok;
	uint64_t req_count_err;
	uint64_t req_count_create_err;
	uint64_t req_count_timeout;
};

/*
 * This is an instance of a client connection, managing
 * an actual client.
 */
struct clt_mgr_conn {
	struct clt_mgr *mgr;

	/* Entry on the manager list */
	TAILQ_ENTRY(clt_mgr_conn) node;

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

extern	int clt_mgr_config(struct clt_mgr *m, const char *host,
	    int port, const char *uri);
extern	int clt_mgr_setup(struct clt_mgr *m, struct clt_thr *th);
extern	int clt_mgr_start(struct clt_mgr *m);

#endif	/* __MGR_H__ */
