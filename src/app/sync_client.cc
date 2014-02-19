#include "rpcc.hh"
#include "common/cpu.hh"
#include "proto/fastrpc_proto_client.hh"
#include "rpc/sync_rpc_transport.hh"
#include "rpc_common/util.hh"
#include "rpc/sync_rpc.hh"
#include "rpc/tcp.hh"
#include "rpc/ib.hh"
#include <thread>
#include <string.h>
#include <sys/prctl.h>

using namespace bench;
using namespace rpc;

#define netstack rpc::ibnet
//#define netstack rpc::tcpnet

const char* host_ = "localhost";

int n_ = 0;
volatile sig_atomic_t stop_ = false;
void handle_alarm(int) {
    stop_ = true;
}

struct check_echo {
    template <typename REQ, typename REPLY>
    void operator()(REQ& req, REPLY& reply) {
        ++n_;
        mandatory_assert(reply.eno() == OK && req.message() == reply.message());
    }
};

void test_async_rtt() {
    bench::rpcc<netstack> c(host_, 8950, 1);
    stop_ = false;
    enum {duration = 5};
    alarm(duration);
    n_ = 0;
    double t0 = rpc::common::now();
    while (!stop_)
        c.echo("hellow world", check_echo());
    std::cout << "test_async_rtt: "<< n_ / (rpc::common::now() - t0) << " rpc/second\n";
    c.drain();
}

int main(int argc, char* argv[]) {
    //assert(prctl(PR_SET_TIMERSLACK, 1000) == 0);
    if (argc > 1)
	host_ = argv[1];
    signal(SIGALRM, handle_alarm);
    test_async_rtt();
    return 0;
}
