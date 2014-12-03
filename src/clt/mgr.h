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
	char *host;
	int port;
	char *uri;
};

extern	int clt_mgr_config(struct clt_mgr *m, struct clt_thr *th,
	    const char *host, int port, const char *uri);
extern	int clt_mgr_setup(struct clt_mgr *m);

#endif	/* __MGR_H__ */
