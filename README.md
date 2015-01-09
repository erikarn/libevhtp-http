This is a simple toolkit for stress testing libevent2 and libevhtp.

srv/ is a simple HTTP server.  The eventual aim is to make the size URL
return a response size that's variable and controllable by a request
parameter.

clt/ is a simple HTTP test client.  It's single threaded for now
and continuously requests the same URL (for now!) from the same
IP (for now!) and to the same IP (for now!).  It's designed to both
test web servers and evaluate/test libevent2+libevhtp.

Why?

.. because there wasn't really any high performance, high throughput,
large number of request workloads.  It's up there with apachebench
in terms of request rate, but it does handle many more concurrent
sockets and it has a more sensible test lifecycle.

