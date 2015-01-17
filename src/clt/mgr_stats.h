#ifndef	__MGR_STATS_H__
#define	__MGR_STATS_H__

struct mgr_stats {
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

#endif	/* __MGR_STATS_H__ */
