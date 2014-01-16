#include "rpcc.hh"
#include "common/cpu.hh"
#include "proto/fastrpc_proto_client.hh"
#include "rpc/sync_tcpconn.hh"

using namespace bench;
using namespace rpc;

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
    bench::rpcc c("localhost", 8950, 1000);
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

void test_echo() {
    bench::rpcc c("localhost", 8950, 1000);
    stop_ = false;
    enum {duration = 5};
    alarm(duration);
    n_ = 0;
    while (!stop_) {
        c.echo("hellow world", check_echo());
        ++n_;
    }
    std::cout << "test_echo: "<< (n_ / duration) << " echo/second\n";
    c.drain();
}

void test_sync_client() {
    bench::TestServiceClient<rpc::sync_tcpconn> client;
    // connect to localhost:8950, using localhost and any port
    client.init("localhost", 8950, "localhost", 0);
    bench::EchoRequest req;
    req.set_message("hello world");
    assert(client.send_echo(req));

    bench::EchoReply reply;
    assert(client.recv_echo(reply));
    assert(reply.message() == req.message());
    printf("test_sync_client: OK\n");
}

int main(int argc, char* argv[]) {
    int index = 0;
    if (argc > 1)
        index = atoi(argv[1]);
    pin(ncore() - index - 1);
    signal(SIGALRM, handle_alarm);
    test_echo();

    test_sync_client();
    return 0;
}
