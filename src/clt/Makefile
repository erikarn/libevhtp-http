PROG=httpclt

SRCS=clt.c mgr.c main.c thr.c mgr_config.c
LDADD=-lpthread

# libevent / libevhtp
CFLAGS+=-I/usr/local/include -g
LDFLAGS+=-L/usr/local/lib/
LDADD+= -levent -levent_pthreads -levhtp

NO_MAN=1

.include <bsd.prog.mk>
