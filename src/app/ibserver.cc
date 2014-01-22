#include "ib.hh"
#include "rpc_common/sock_helper.hh"

static const size_t size = 20;
static char b[size];

static void process_infb_event(infb_ev_watcher* w, int flags, infb_conn* c) {
    static double t0;
    if (flags & INFB_EV_READ) {
        static int iters = 0;
	c->read(b, sizeof(b));
	char eb[size];
	sprintf(eb, "c_%d", iters);
	assert(strcmp(eb, b) == 0);
	++iters;
	w->start(INFB_EV_WRITE);
    } else if (flags & INFB_EV_WRITE) {
        static int iters = 0;
	sprintf(b, "s_%d", iters++);
	c->write(b, sizeof(b));
	w->start(INFB_EV_READ);
    }
}

int main(int, char*[]) {
    infb_conn conn;
    // the first infiniband port is 1
    int ib_port = 1;
    bool use_event = false;
    int sl = 0;

    conn.create(infb_provider::make(), ib_port, use_event, sl);
    conn.local_address().dump(stdout);

    int sfd = rpc::common::sock_helper::listen(8181);
    assert(sfd >= 0);
    int fd = rpc::common::sock_helper::accept(sfd);
    assert(fd >= 0);
    close(sfd);

    infb_sockaddr remote;
    assert(read(fd, &remote, sizeof(remote)) == sizeof(remote));

    const infb_sockaddr& local = conn.local_address();
    assert(write(fd, &local, sizeof(local)) == sizeof(local));

    remote.dump(stdout);
    assert(conn.connect(remote) == 0);

    infb_ev_watcher w;
    w.set(process_infb_event);
    w.start(INFB_EV_READ);

    conn.set_ev_watcher(&w);
    while (true) {
	conn.loop_once();
    }
    return 0;
}
