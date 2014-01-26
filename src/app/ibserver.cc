#include "ib.hh"

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
	w->set(INFB_EV_WRITE);
    } else if (flags & INFB_EV_WRITE) {
        static int iters = 0;
	sprintf(b, "s_%d", iters++);
	c->write(b, sizeof(b));
	w->set(INFB_EV_READ);
    }
}

int main(int, char*[]) {
    infb_loop* loop = infb_loop::make(infb_provider::default_instance());
    infb_server s;
    s.listen(8181);
    infb_ev_watcher* w = loop->ev_watcher(s.accept(loop));
    w->set(process_infb_event);
    w->set(INFB_EV_READ);
    while (true)
	loop->loop_once();
    return 0;
}
