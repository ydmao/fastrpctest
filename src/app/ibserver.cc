#include "rpc/ib.hh"
#include <string>
#include "rpc/libev_loop.hh"

using namespace rpc;

struct client {
    static constexpr size_t size = 20;
    char b[size];
    int nr_;
    int nw_;
    client(infb_conn* c) : nr_(0), nw_(0), c_(c) {
    }
    ~client() {
	delete c_;
    }
   bool read(infb_conn* c) {
        if (c->read(b, sizeof(b)) != sizeof(b))
	    return false;
        char eb[size];
        sprintf(eb, "c_%d", nr_++);
        assert(strcmp(eb, b) == 0);
	return true;
    }
    bool write(infb_conn* c) {
        sprintf(b, "s_%d", nw_++);
        if (c->write(b, sizeof(b)) != sizeof(b))
	    return false;
	return true;
    }
    void event_handler(infb_async_conn* ac, int flags) {
        if (flags & ev::READ) {
	    if (!read(c_)) {
		delete this;
		return;
	     }
	    ac->eselect(ev::WRITE);
        } else if (flags & ev::WRITE) {
   	    if (!write(c_)) {
		delete this;
		return;
	    }
	    ac->eselect(ev::READ);
        }
    }
  private:
    infb_conn* c_;
};

int main(int argc, char* argv[]) {
    infb_conn_type type = INFB_CONN_ASYNC;
    if (argc > 1)
	type = make_infb_type(argv[1]);
    infb_server s;
    s.listen(8181);
    while (true) {
        infb_conn* c = s.accept(type);
        client* clt = new client(c);
        if (type != INFB_CONN_ASYNC) {
	    while (true) {
	        clt->read(c);
	        clt->write(c);
	    }
	} else {
	    std::thread t([=]{
		    rpc::nn_loop* loop = rpc::nn_loop::get_tls_loop();
	            using std::placeholders::_1;
	            using std::placeholders::_2;
		    infb_async_conn* ac = static_cast<infb_async_conn*>(c);
		    ac->register_callback(std::bind(&client::event_handler, clt, _1, _2), ev::READ);
		    loop->enter();
                    while (true)
	                loop->run_once();
		    loop->leave();
	        });
	    t.detach();
	}
    }
    return 0;
}
