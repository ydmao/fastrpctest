#include "rpcc.hh"
#include "common/cpu.hh"

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
    std::cout << (n_ / duration) << " echo/second\n";
    c.drain();
}

int main(int argc, char* argv[]) {
    pin(ncore() - 1);
    signal(SIGALRM, handle_alarm);
    test_echo();
    return 0;
}
