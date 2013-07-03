#include "bench.pb.h"
#include "rpc_common/compiler.hh"
#include "rpc/rpc_server.hh"
#include "rpcc.hh"
#include "common/cpu.hh"
#include <iostream>
#include <signal.h>
#include <boost/program_options.hpp>

namespace bpo = boost::program_options;

namespace bench {

volatile sig_atomic_t terminated_ = 0;
void handle_term(int) {
    terminated_ = true;
}

struct server {
    server(int port) : rpcs_(port, this) {
    }
    void nb_nop(rpc::grequest<ProcNumber::nop>& q, rpc::async_rpcc*, uint64_t) {
        q.execute(OK);
    }
    void nb_echo(rpc::grequest<ProcNumber::echo>& q, rpc::async_rpcc*, uint64_t) {
        q.reply_.set_message(q.req_.message());
        q.execute(OK);
    }
  private:
    rpc::async_rpc_server<server, 0, true> rpcs_;
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
        cerr << "Unknown option " << e.get_option_name() << ".\n" << desc << "\n";
        return -1;
    }

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    mandatory_assert(signal(SIGTERM, bench::handle_term) == 0);
    bench::server s(port);
    std::cout << argv[0] << " listening at port " << port << "\n";
    auto loop = rpc::nn_loop::get_tls_loop();
    loop->enter();
    while (!bench::terminated_)
        loop->run_once();
    loop->leave();
    return 0;
}
