#include "rpc/ib.hh"
#include "rpc_common/util.hh"
#include "rpc/libev_loop.hh"

const size_t size = 20;
static char b[size];

using namespace rpc;

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

static void process_infb_event(infb_async_conn* c, int flags) {
    if (flags & ev::READ) {
	read(c);
	c->eselect(ev::WRITE);
    } else if (flags & ev::WRITE) {
	write(c);
	c->eselect(ev::READ);
    }
}

int main(int argc, char* argv[]) {
    infb_conn_type type = INFB_CONN_ASYNC;
    if (argc > 1)
	type = make_infb_type(argv[1]);

    infb_conn* c = infb_connect("192.168.100.11", 8181, type);
    if (type != INFB_CONN_ASYNC) {
        while (true) {
            write(c);
            read(c);
        }
    } else {
	rpc::nn_loop* loop = rpc::nn_loop::get_tls_loop();
	using std::placeholders::_1;
	using std::placeholders::_2;
	infb_async_conn* ac = static_cast<infb_async_conn*>(c);
	ac->register_callback(process_infb_event, ev::READ);
        write(c);
	loop->enter();
        while (true)
	    loop->run_once();
	loop->leave();
    }

    return 0;
}
