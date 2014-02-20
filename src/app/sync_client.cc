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
int port_ = 8950;

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
    bench::rpcc<netstack> c(host_, port_, 1);
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

#if 1
typedef rpc::buffered_sync_transport<netstack> buffered_transport;
typedef rpc::sync_rpc_transport<buffered_transport> rpc_transport;
#else
typedef rpc::direct_sync_transport<netstack> direct_transport;
typedef rpc::sync_rpc_transport<direct_transport> rpc_transport;
#endif

void test_sync_rtt() {
    bench::TestServiceClient<rpc_transport> client;
    // connect to localhost:8950, using localhost and any port
    client.set_address(host_, port_, "0.0.0.0");
    bench::EchoRequest req;
    bench::EchoReply reply;
    req.set_message("hello world");

    // make sure we are connected    
    assert(client.send_echo(req));
    assert(client.recv_echo(reply));

    int n = 0;
    double t0 = rpc::common::now();
    for (; rpc::common::now() - t0 < 5; ++n) {
        assert(client.send_echo(req));
	//reply.Clear();
        assert(client.recv_echo(reply));
        assert(reply.message() == req.message());
    }
    printf("test_sync_rtt: %.1f us/rpc\n", (rpc::common::now() - t0) * 1000000 / n);
}

int main(int argc, char* argv[]) {
    //assert(prctl(PR_SET_TIMERSLACK, 1000) == 0);
    if (argc > 1)
	host_ = argv[1];
    if (argc > 2)
	port_ = atoi(argv[2]);
    signal(SIGALRM, handle_alarm);
    test_async_rtt();
    return 0;
}
