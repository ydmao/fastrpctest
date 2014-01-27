#include "ib.hh"
#include <string>

struct client {
    static constexpr size_t size = 20;
    char b[size];
    int nr_;
    int nw_;
    client() : nr_(0), nw_(0) {
    }
    void read(infb_conn* c) {
        c->read(b, sizeof(b));
        char eb[size];
        sprintf(eb, "c_%d", nr_++);
        assert(strcmp(eb, b) == 0);
    }
    void write(infb_conn* c) {
        sprintf(b, "s_%d", nw_++);
        c->write(b, sizeof(b));
    }
    void event_handler(infb_ev_watcher* w, int flags) {
        if (flags & INFB_EV_READ) {
	    read(w->conn());
	    w->set(INFB_EV_WRITE);
        } else if (flags & INFB_EV_WRITE) {
   	    write(w->conn());
	    w->set(INFB_EV_READ);
        }
    }
};

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
	client* clt = new client();
	while (true) {
	    clt->read(c);
	    clt->write(c);
	}
    } else if (type == "async") {
	while (true) {
            infb_loop* loop = infb_loop::make(infb_provider::default_instance());
	    infb_conn* c = s.accept(loop);
	    std::thread t([=]{
                infb_ev_watcher* w = loop->ev_watcher(c);
	        client* clt = new client;
	        using std::placeholders::_1;
	        using std::placeholders::_2;
                w->set(std::bind(&client::event_handler, clt, _1, _2));
                w->set(INFB_EV_READ);
                while (true)
	            loop->loop_once();
	    });
	    t.detach();
	}
    } else {
	fprintf(stderr, "Unknown connection type %s\n", type.c_str());
	fprintf(stderr, "Usage: %s poll|int|async\n", argv[0]);
    }
    return 0;
}
