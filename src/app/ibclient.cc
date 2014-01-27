#include "ib.hh"
#include "rpc_common/util.hh"

const size_t size = 20;
static char b[size];

static void read(infb_conn* c) {
    static double t0;
    static int iters = 0;
    assert(c->read(b, sizeof(b)) == sizeof(b));
    char eb[size];
    sprintf(eb, "s_%d", iters);
    //fprintf(stderr, "expected %s, actual %s\n", eb, b);
    assert(strcmp(eb, b) == 0);
    if (iters == 0)
        t0 = rpc::common::now();
    ++iters;
    if (iters == 200000) {
        double t = rpc::common::now() - t0;
        fprintf(stderr, "completed %d iterations in %.2f seconds, latency %.1f us\n",
    	        iters, t, t * 1000000 / iters);
	delete c;
        exit(0);
    }
}

static void write(infb_conn*c ) {
    static int iters = 0;
    sprintf(b, "c_%d", iters++);
    assert(c->write(b, sizeof(b)) == sizeof(b));
}

static void process_infb_event(infb_ev_watcher* w, int flags) {
    if (flags & INFB_EV_READ) {
	read(w->conn());
	w->set(INFB_EV_WRITE);
    } else if (flags & INFB_EV_WRITE) {
	write(w->conn());
	w->set(INFB_EV_READ);
    }
}

int main(int argc, char* argv[]) {
    std::string type("async");
    if (argc > 1)
	type.assign(argv[1]);

    if (type == "poll" || type == "int") {
	infb_conn_factory* f;
	if (type == "poll")
	    f = infb_poll_factory::default_instance();
	else
	    f = infb_interrupt_factory::default_instance();
        infb_conn* c = infb_client::connect("192.168.100.11", 8181, f);
	while (true) {
	    write(c);
	    read(c);
        }
    } else if (type == "async") {
        infb_loop* loop = infb_loop::make(infb_provider::default_instance());
        infb_conn* c = infb_client::connect("192.168.100.11", 8181, loop);
        infb_ev_watcher* w = loop->ev_watcher(c);
        w->set(process_infb_event);
        w->set(INFB_EV_READ);
        write(c);
        while (true)
	    loop->loop_once();
    } else {
	fprintf(stderr, "Unknown connection type %s\n", type.c_str());
	fprintf(stderr, "Usage: %s poll|int|async\n", argv[0]);
    }

    return 0;
}
