#ifndef BENCH_RPCC_HH
#define BENCH_RPCC_HH

#include "rpc/async_rpcc_helper.hh"
#include "proto/fastrpc_proto.hh"

namespace bench {

template <typename T>
struct rpcc : public rpc::async_batched_rpcc<T> {
    rpcc(const char *host, int port, int w)
        : rpc::async_batched_rpcc<T>(host, port, w) {
    }
    template <typename F>
    void nop(F cb) {
        auto g = new rpc::gcrequest<ProcNumber::nop>(cb);
        this->call(g);
    }
    template <typename F>
    void echo(const std::string& v, F cb) {
        auto g = new rpc::gcrequest<ProcNumber::echo>(cb);
        g->req_.set_message(v);
        this->call(g);
    }
};

}

#endif
