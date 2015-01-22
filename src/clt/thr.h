#ifndef	__THR_H__
#define	__THR_H__

struct clt_thr;

struct clt_thr {
	int t_tid;		/* thread id; local */
	struct clt_mgr *t_m;
	pthread_t t_thr;
	evbase_t *t_evbase;
	evhtp_t  *t_htp;

	/* Previous statistics */
	pthread_mutex_t prev_stats_mtx;
	struct mgr_stats prev_stats;
};

extern	int clt_thr_setup(struct clt_thr *th, int tid);
extern	void clt_thr_free(struct clt_thr *th);

#endif	/* __THR_H__ */
