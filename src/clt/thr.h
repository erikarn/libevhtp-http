#ifndef	__THR_H__
#define	__THR_H__

struct clt_thr {
	int t_tid;		/* thread id; local */
	pthread_t t_thr;
	evbase_t *t_evbase;
	evhtp_t  *t_htp;

	/*
	 * For now this will be like apachebench - really quite stupid.
	 * Later on we may wish to support some kind of module or
	 * modules that implement a slight smarter testing policy.
	 */

#if 0
	/* How many open connections */
	int nconn;

	/* how many to attempt to open */
	int target_nconn;
	char *host;
	int port;
	char *uri;
#endif
};

extern	int clt_thr_setup(struct clt_thr *th, int tid);

#endif	/* __THR_H__ */
