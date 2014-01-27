#include "ib.hh"
#include "rpc_common/util.hh"

const size_t size = 4096;
static char b[size];

static void write(infb_conn* c) {
    static double t0;
    static int iters = 0;
    if (iters == 0)
	t0 = rpc::common::now();
    do {
        sprintf(b, "c_%d", iters++);
    } while (c->write(b, sizeof(b)) == sizeof(b));
    --iters;
    if (iters >= 400000) {
        double t = rpc::common::now() - t0;
        fprintf(stderr, "completed %d iterations in %.2f seconds, bw %.1f Mbps\n",
    	        iters, t, iters * size * 8 / t / (1<<20));
        exit(0);
    }
}

static void process_infb_event(infb_ev_watcher* w, int flags) {
    if (flags & INFB_EV_WRITE)
	write(w->conn());
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
	while (true)
	    write(c);
    } else if (type == "async") {
        infb_loop* loop = infb_loop::make(infb_provider::default_instance());
        infb_conn* c = infb_client::connect("192.168.100.11", 8181, loop);
        infb_ev_watcher* w = loop->ev_watcher(c);
        w->set(process_infb_event);
        w->set(INFB_EV_WRITE);
        while (true)
	    loop->loop_once();
    } else {
	fprintf(stderr, "Unknown connection type %s\n", type.c_str());
	fprintf(stderr, "Usage: %s poll|int|async\n", argv[0]);
    }

    return 0;
}
