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

struct check_nop {
    template <typename REQ, typename REPLY>
    void operator()(REQ&, REPLY& reply) {
        ++n_;
        mandatory_assert(reply.eno() == OK);
    }
};

struct check_echo {
    template <typename REQ, typename REPLY>
    void operator()(REQ& req, REPLY& reply) {
        ++n_;
        mandatory_assert(reply.eno() == OK && req.message() == reply.message());
    }
};

void test_nop() {
    bench::rpcc<netstack> c(host_, 8950, 1000);
    stop_ = false;
    enum {duration = 5};
    alarm(duration);
    n_ = 0;
    while (!stop_) {
        c.nop(check_nop());
        ++n_;
    }
    std::cout << (n_ / duration) << " nop/second\n";
    c.drain();
}

void test_async_rtt() {
    bench::rpcc<netstack> c(host_, 8950, 1);
    stop_ = false;
    enum {duration = 5};
    alarm(duration);
    n_ = 0;
    double t0 = rpc::common::now();
    while (!stop_)
        c.echo("hellow world", check_echo());
    std::cout << "test_async_rtt: "<< (1000000*(rpc::common::now() - t0)/n_) << " us/rpc\n";
    c.drain();
}

typedef rpc::buffered_sync_transport<netstack> buffered_transport;
typedef rpc::sync_rpc_transport<buffered_transport> rpc_transport;

void test_sync_client() {
    bench::TestServiceClient<rpc_transport> client;
    // connect to localhost:8950, using localhost and any port
    client.set_address(host_, 8950, "0.0.0.0");
    bench::EchoRequest req;
    req.set_message("hello world");
    assert(client.send_echo(req));

    bench::EchoReply reply;
    assert(client.recv_echo(reply));
    assert(reply.message() == req.message());
    printf("test_sync_client: OK\n");
}

void test_sync_rtt() {
    bench::TestServiceClient<rpc_transport> client;
    // connect to localhost:8950, using localhost and any port
    client.set_address(host_, 8950, "0.0.0.0");
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

void test_sync_threaded_rtt() {
    rpc_transport c;
    c.set_address(host_, 8950, "0.0.0.0");
    assert(c.connect());
    double sum = 0;
    int n = 0;
    std::thread th([&]{ 
	    rpc::rpc_header h;
	    bench::EchoReply r;
	    while (c.read_reply(h, r)) {
		++n;
	        double t = 0;
		memcpy(&t, r.message().data(), sizeof(double));
		asm volatile("");
		sum += rpc::common::now() - t;
	    }
	});

    bench::EchoRequest req;
    double t0 = rpc::common::now();
    for (; rpc::common::now() - t0 < 5; ) {
	double t = rpc::common::now();
	req.mutable_message()->assign((char*)&t, sizeof(t));
	c.send_request(ProcNumber::echo, n, req, true);
	usleep(1);
    }
    c.shutdown();
    th.join();
    printf("test_sync_threaded_rtt: %.1f us/rpc\n", sum*1000000/n);
}

void test_message() {
    bench::EchoRequest req;
    req.set_message("hello world");
    char buf[4096];
    assert(req.SerializeToArray((uint8_t*)buf, sizeof(buf)));

    req.Clear();
    assert(req.ParseFromArray((uint8_t*)buf, sizeof(buf)));

    bench::nb_EchoRequest nbreq;
    assert(req.ParseFromArray((uint8_t*)buf, sizeof(buf)));
    printf("test_message: OK\n");
}

void test_large_rpc() {
    bench::rpcc<netstack> c(host_, 8950, 1);
    stop_ = false;
    char buf[8192];
    memset(buf, 'a', sizeof(buf));
    buf[sizeof(buf) - 1] = 0;

    enum {duration = 5};
    alarm(duration);
    n_ = 0;
    double t0 = rpc::common::now();
    while (!stop_)
        c.echo(buf, check_echo());
    std::cout << "test_large_rpc: "<< (1000000*(rpc::common::now() - t0)/n_) << " us/rpc\n";
    c.drain();
}

int main(int argc, char* argv[]) {
    int index = 0;
    if (argc > 1)
        index = atoi(argv[1]);
    if (argc > 2)
	host_ = argv[2];
    pin(ncore() - index - 1);
    signal(SIGALRM, handle_alarm);
    test_large_rpc();
    test_message();
    test_async_rtt();
    test_sync_threaded_rtt();
    test_sync_client();
    test_sync_rtt();
    return 0;
}
