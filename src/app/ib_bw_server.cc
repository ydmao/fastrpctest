#include "ib.hh"
#include <string>

static const size_t size = 4096;
static char b[size];

static void read(infb_conn* c) {
    static int iters = 0;
    c->read(b, sizeof(b));
    char eb[size];
    sprintf(eb, "c_%d", iters++);
    //fprintf(stderr, "got %s, expected %s\n", b, eb);
    assert(strcmp(eb, b) == 0);
}

static void process_infb_event(infb_ev_watcher* w, int flags) {
    if (flags & INFB_EV_READ)
	read(w->conn());
}

int main(int argc, char* argv[]) {
    std::string type("async");
    if (argc > 1)
	type.assign(argv[1]);

    infb_server s;
    s.listen(8181);

    if (type == "poll" || type == "int") {
	infb_conn_factory* f;
	if (type == "poll")
	    f = infb_poll_factory::default_instance();
	else
	    f = infb_interrupt_factory::default_instance();
	infb_conn* c = s.accept(f);
	while (true)
	    read(c);
    } else if (type == "async") {
        infb_loop* loop = infb_loop::make(infb_provider::default_instance());
        infb_ev_watcher* w = loop->ev_watcher(s.accept(loop));
        w->set(process_infb_event);
        w->set(INFB_EV_READ);
        while (true)
	    loop->loop_once();
    } else {
	fprintf(stderr, "Unknown connection type %s\n", type.c_str());
	fprintf(stderr, "Usage: %s poll|int|async\n", argv[0]);
    }
    return 0;
}
