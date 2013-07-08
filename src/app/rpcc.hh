#ifndef BENCH_RPCC_HH
#define BENCH_RPCC_HH

#include "rpc/async_rpcc_helper.hh"
#include "proto/fastrpc_proto.hh"

namespace bench {

struct rpcc : public rpc::async_batched_rpcc {
    rpcc(const char *host, int port, int w)
        : rpc::async_batched_rpcc(new rpc::async_rpcc(host, port, NULL, NULL), w) {
    }
    rpcc(rpc::async_rpcc* cl, int w) : rpc::async_batched_rpcc(cl, w) {
    }
    template <typename F>
    void nop(F callback) {
        auto g = new rpc::gcrequest<ProcNumber::nop, F>(callback);
        cl_->call(g);
        winctrl();
    }
    template <typename F>
    void echo(const std::string& v, F callback) {
        auto g = new rpc::gcrequest<ProcNumber::echo, F>(callback);
        g->req_.set_message(v);
        cl_->call(g);
        winctrl();
    }
};

}

#endif
