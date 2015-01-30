PROG=httpclt

SRCS=clt.c mgr.c main.c thr.c mgr_config.c mgr_stats.c
LDADD=-lpthread

# libevent / libevhtp
CFLAGS=-I/usr/local/include -I/home/adrian/local/include -g
LDFLAGS+=-L/usr/local/lib/ -L/home/adrian/local/lib
LDADD+= -lcrypto -lssl -levent -levent_pthreads -levent_openssl -levhtp

NO_MAN=1

.include <bsd.prog.mk>
