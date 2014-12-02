#ifndef	__CLT_H__
#define	__CLT_H__

/*
 * A client request will have a connection (con) to an IP address, and then
 * one or more outstanding HTTP requests.
 *
 * I'm not sure if libevhtp supports HTTP pipelining at the present time,
 * so let's just assume a single request at a time.
 */
struct client_req {
	evhtp_connection_t *con;
	evhtp_request_t *req;
	struct clt_thr *thr;

	/* Connection details */
	char *host;
	int port;

	/* Request URI */
	char *uri;

	/*
	 * How many requests to issue before this client
	 * request is torn down.
	 */
	int nreq;

	/* How much data was read */
	size_t cur_read_ptr;

#if 0
	/* Read buffer - mostly just scratch-space to read into */
	struct {
		char *buf;
		int size;
	} buf;
#endif
};

extern	void clt_conn_destroy(struct client_req *req);
extern	void clt_req_destroy(struct client_req *req);
extern	struct client_req * clt_conn_create(struct clt_thr *thr,
	    const char *host, int port);
extern	int clt_req_create(struct client_req *req, const char *uri);

#endif
