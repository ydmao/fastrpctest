#include "ib.hh"
#include "rpc_common/sock_helper.hh"

const size_t size = 4096;
static char b[size];

static void process_infb_event(infb_ev_watcher* w, int flags, infb_conn* c) {
    static double t0;
    if (flags & INFB_EV_READ) {
        static int iters = 0;
	assert(c->read(b, sizeof(b)) == sizeof(b));
	char eb[size];
	sprintf(eb, "s_%d", iters);
	//fprintf(stderr, "expected %s, actual %s\n", eb, b);
	assert(strcmp(eb, b) == 0);
	if (iters == 0)
	    t0 = now();
	++iters;
	if (iters == 200000) {
	    double t = now() - t0;
	    fprintf(stderr, "completed %d iterations in %.2f seconds, latency %.1f us\n",
		    iters, t, t * 1000000 / iters);
	    exit(0);
	}
	w->start(INFB_EV_WRITE);
    } else if (flags & INFB_EV_WRITE) {
	static int iters = 1;
	sprintf(b, "c_%d", iters++);
	assert(c->write(b, sizeof(b)) == sizeof(b));
	w->start(INFB_EV_READ);
    }
}

int main(int, char*[]) {
    infb_conn conn;
    // the first infiniband port is 1
    int ib_port = 1;
    int rx_depth = 1000;
    bool use_event = false;
    int sl = 0;
    ibv_mtu mtu = IBV_MTU_1024;

    conn.create(NULL, ib_port, size, rx_depth, use_event, sl, mtu);
    conn.local_address().dump(stdout);

    int fd = rpc::common::sock_helper::connect("192.168.100.11", 8181);
    assert(fd >= 0);
    const infb_sockaddr& local = conn.local_address();
    assert(write(fd, &local, sizeof(local)) == sizeof(local));
    infb_sockaddr remote;
    assert(read(fd, &remote, sizeof(remote)) == sizeof(remote));

    remote.dump(stdout);
    assert(conn.connect(remote) == 0);

    infb_ev_watcher w;
    w.set(process_infb_event);
    w.start(INFB_EV_READ);
    conn.set_ev_watcher(&w);
    sprintf(b, "c_0");
    conn.write(b, sizeof(b));
    while (true) {
	conn.loop_once();
    }

    return 0;
}
