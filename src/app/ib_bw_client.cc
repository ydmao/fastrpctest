#include "rpc/ib.hh"
#include "rpc_common/util.hh"
#include "rpc/libev_loop.hh"

using namespace rpc;

const size_t size = 4096;
static char b[size];

static void write(infb_conn* c) {
    static double t0;
    static int iters = 0;
    if (iters == 0)
	t0 = rpc::common::now();
    ssize_t r;
    do {
        sprintf(b, "c_%d", iters++);
    } while ((r = c->write(b, sizeof(b))) == sizeof(b));
    if (r > 0) {
	fprintf(stderr, "sent partial message\n");
	assert(0);
    }
    --iters;
    if (iters >= 400000) {
        double t = rpc::common::now() - t0;
        fprintf(stderr, "completed %d iterations in %.2f seconds, bw %.1f Mbps\n",
    	        iters, t, iters * size * 8 / t / (1<<20));
        exit(0);
    }
}

static void process_infb_event(infb_async_conn* c, int flags) {
    if (flags & ev::WRITE)
	write(c);
}

int main(int argc, char* argv[]) {
    infb_conn_type type = INFB_CONN_ASYNC;
    if (argc > 1)
	type = make_infb_type(argv[1]);

    infb_conn* c = infb_connect("192.168.100.11", 8181, type);
    if (type != INFB_CONN_ASYNC) {
	while (true)
	    write(c);
    } else {
	rpc::nn_loop* loop = rpc::nn_loop::get_tls_loop();
	using std::placeholders::_1;
	using std::placeholders::_2;
	infb_async_conn* ac = static_cast<infb_async_conn*>(c);
	ac->register_loop(loop->ev_loop(), process_infb_event, ev::WRITE);
	loop->enter();
        while (true) {
	    ac->drain();
	    loop->run_once();
	}
	loop->leave();
    }
    return 0;
}
