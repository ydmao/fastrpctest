#include "rpc/ib.hh"
#include <string>
#include "rpc/libev_loop.hh"
#include "rpc_common/util.hh"

using namespace rpc;

struct client {
    static constexpr size_t size = 4096;
    char b[size];
    int nr_;
    int off_;
    client(infb_conn* c) : nr_(0), c_(c), off_(0) {
    }
    ~client() {
	delete c_;
    }
    bool read() {
        ssize_t r = c_->read(b + off_, sizeof(b) - off_);
	if (r < 0) {
	    perror("client::read");
	    return false;
	}
	off_ += r;
	if (off_ != sizeof(b))
	    return true;
	static double last = 0;
	if (rpc::common::now() - last > 1) {
	    fprintf(stderr, "%d\n", nr_);
	    last = rpc::common::now();
	}
	//fprintf(stderr, "reading %d\n", nr_);
        char eb[size];
        sprintf(eb, "c_%d", nr_++);
        assert(strcmp(eb, b) == 0);
	off_ = 0;
	return true;
    }
    bool event_handler(infb_async_conn*, int flags) {
        if (flags & ev::READ)
	    if (!read()) {
		delete this;
		return true;
	    }
	return false;
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
	    while (clt->read());
	    delete clt;
        } else  {
	    std::thread t([=]{
		    rpc::nn_loop* loop = rpc::nn_loop::get_tls_loop();
	            using std::placeholders::_1;
	            using std::placeholders::_2;
		    infb_async_conn* ac = static_cast<infb_async_conn*>(c);
		    ac->register_callback(std::bind(&client::event_handler, clt, _1, _2), ev::READ);
		    loop->enter();
                    while (loop->has_edge_triggered())
	                loop->run_once();
		    fprintf(stderr, "exiting\n");
		    loop->leave();
	        });
  	    t.detach();
	}
    }
    return 0;
}
