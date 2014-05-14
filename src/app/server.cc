#include "proto/fastrpc_proto.hh"
#include "proto/fastrpc_proto_server.hh"
#include "rpc_common/compiler.hh"
#include "rpc/rpc_server.hh"
#include "rpcc.hh"
#include "common/cpu.hh"
#include "rpc/tcp.hh"
#include "rpc/ib.hh"
#include <iostream>
#include <signal.h>
#include <boost/program_options.hpp>

namespace bpo = boost::program_options;

//#define netstack rpc::ibnet
#define netstack rpc::tcpnet

namespace bench {

volatile sig_atomic_t terminated_ = 0;
void handle_term(int) {
    terminated_ = true;
}

template <typename T>
struct server : public TestServiceInterface<T, true> {
    server() {
    }
#if 0
    void nop(rpc::grequest<ProcNumber::nop, false>* q, uint64_t) {
        q->execute(OK);
    }
    void echo(rpc::grequest<ProcNumber::echo, false>* q, uint64_t) {
        q->reply_.set_message(q->req_.message());
        q->execute(OK);
    }
#endif
    void nop(rpc::grequest<ProcNumber::nop, true>& q, uint64_t) {
        q.execute(OK);
    }
    void echo(rpc::grequest<ProcNumber::echo, true>& q, uint64_t) {
        q.reply_.set_message(q.req_.message());
        q.execute(OK);
    }
    void client_failure(rpc::async_rpcc<T>*) {
        // usually nop
    }
};

} // namespace pcloud

int main(int argc, char* argv[]) {
    pin(0);
    int port;
    bpo::options_description desc("Allowed options");
    try {
        desc.add_options()
            ("port,p", bpo::value<int>(&port)->default_value(8950));
        bpo::variables_map vm;
        bpo::store(bpo::parse_command_line(argc, argv, desc), vm);
        bpo::notify(vm);
    } catch (bpo::unknown_option &e) {
        std::cerr << "Unknown option " << e.get_option_name() << ".\n" << desc << "\n";
        return -1;
    }
    mandatory_assert(signal(SIGTERM, bench::handle_term) == 0);
    rpc::async_rpc_server<netstack> rpcs(port, "0.0.0.0");
    bench::server<netstack> s;
    rpcs.register_service(&s);
    std::cout << argv[0] << " listening at port " << port << "\n";
    auto loop = rpc::nn_loop::get_tls_loop();
    loop->enter();
    while (!bench::terminated_)
        loop->run_once();
    loop->leave();
    return 0;
}
