#ifndef	__THR_H__
#define	__THR_H__

struct clt_thr {
	int t_tid;		/* thread id; local */
	struct clt_mgr *t_m;
	pthread_t t_thr;
	evbase_t *t_evbase;
	evhtp_t  *t_htp;
};

extern	int clt_thr_setup(struct clt_thr *th, int tid);

#endif	/* __THR_H__ */
