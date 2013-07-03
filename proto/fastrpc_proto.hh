#ifndef FASTRPC_PROTO_HH
#define FASTRPC_PROTO_HH 1
#include "bench.pb.h"

namespace rpc {

#define appns bench
struct app_param {
    typedef appns::ErrorCode ErrorCode;
    typedef appns::ProcNumber ProcNumber;
    static constexpr uint32_t nproc = appns::ProcNumber::nproc;
};

#define RPC_FOR_EACH_CLIENT_MESSAGE(M, ...)			\
    M(0, nop, NopRequest, NopReply, ## __VA_ARGS__) \
    
#define RPC_FOR_EACH_INTERCONNECT_MESSAGE(M, ...)
};

#endif
