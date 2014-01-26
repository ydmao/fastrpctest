#include "ib.hh"
#include "rpc_common/util.hh"

const size_t size = 20;
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
	    t0 = rpc::common::now();
	++iters;
	if (iters == 200000) {
	    double t = rpc::common::now() - t0;
	    fprintf(stderr, "completed %d iterations in %.2f seconds, latency %.1f us\n",
		    iters, t, t * 1000000 / iters);
	    exit(0);
	}
	w->set(INFB_EV_WRITE);
    } else if (flags & INFB_EV_WRITE) {
	static int iters = 1;
	sprintf(b, "c_%d", iters++);
	assert(c->write(b, sizeof(b)) == sizeof(b));
	w->set(INFB_EV_READ);
    }
}

int main(int, char*[]) {
    infb_loop* loop = infb_loop::make(infb_provider::default_instance());
    infb_conn* c = infb_client::connect("192.168.100.11", 8181, loop);
    infb_ev_watcher* w = loop->ev_watcher(c);
    w->set(process_infb_event);
    w->set(INFB_EV_READ);
    sprintf(b, "c_0");
    c->write(b, sizeof(b));
    while (true)
	loop->loop_once();
    return 0;
}
