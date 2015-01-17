#ifndef	__MGR_STATS_H__
#define	__MGR_STATS_H__

struct mgr_stats {
	/* How many open connections - gauge, not counter */
	int nconn;

	uint64_t conn_count;
	uint64_t conn_closing_count;

	uint64_t req_count;
	uint64_t req_count_ok;
	uint64_t req_count_err;
	uint64_t req_count_create_err;
	uint64_t req_count_timeout;

	uint64_t req_statustype_200;
	uint64_t req_statustype_302;
	uint64_t req_statustype_other;
};

extern	void mgr_stats_copy(const struct mgr_stats *src, struct mgr_stats *dst);
extern	void mgr_stats_diff(const struct mgr_stats *sfrom,
	    const struct mgr_stats *sto, struct mgr_stats *res);
extern	void mgr_stats_add(const struct mgr_stats *from, struct mgr_stats *sto);

#endif	/* __MGR_STATS_H__ */
