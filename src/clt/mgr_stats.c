#include <sys/types.h>

#include "mgr_stats.h"

void
mgr_stats_copy(const struct mgr_stats *src, struct mgr_stats *dst)
{

	*dst = *src;
}

void
mgr_stats_diff(const struct mgr_stats *sfrom, const struct mgr_stats *sto,
    struct mgr_stats *res)
{

	res->nconn = sto->nconn - sfrom->nconn;

	res->conn_count = sto->conn_count - sfrom->conn_count;
	res->conn_closing_count = sto->conn_closing_count - sfrom->conn_closing_count;

	res->req_count = sto->req_count - sfrom->req_count;
	res->req_count_ok = sto->req_count_ok - sfrom->req_count_ok;
	res->req_count_err  = sto->req_count_err - sfrom->req_count_err;
	res->req_count_create_err = sto->req_count_create_err - sfrom->req_count_create_err;
	res->req_count_timeout = sto->req_count_timeout - sfrom->req_count_timeout;

	res->req_statustype_200 = sto->req_statustype_200 - sfrom->req_statustype_200;
	res->req_statustype_302  = sto->req_statustype_302 - sfrom->req_statustype_302;
	res->req_statustype_other  = sto->req_statustype_other - sfrom->req_statustype_other;
}

void
mgr_stats_add(const struct mgr_stats *sfrom, struct mgr_stats *sto)
{

	sto->nconn += sfrom->nconn;
	sto->conn_count += sfrom->conn_count;
	sto->conn_closing_count += sfrom->conn_closing_count;

	sto->req_count += sfrom->req_count;
	sto->req_count_ok += sfrom->req_count_ok;
	sto->req_count_err += sfrom->req_count_err;
	sto->req_count_create_err += sfrom->req_count_create_err;
	sto->req_count_timeout += sfrom->req_count_timeout;

	sto->req_statustype_200 += sfrom->req_statustype_200;
	sto->req_statustype_302 += sfrom->req_statustype_302;
	sto->req_statustype_other += sfrom->req_statustype_other;
}
