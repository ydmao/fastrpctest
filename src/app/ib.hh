#pragma once
#include <infiniband/verbs.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <algorithm>
#include <sys/time.h>
#include <queue>
#include <thread>
#include <stdarg.h>
#include <mutex>
#include <ev++.h>

#include "rpc_common/sock_helper.hh"
#include "compiler/str.hh"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define CHECK(x) { if(!(x)){ \
                     fprintf(stderr, "CHECK(%s) failed %s:%d\n", \
                             #x, __FILE__, __LINE__); perror("check"); exit(1); } }

enum { nodebug = 1 };

inline void dbg(const char* fmt, ...) {
    if (nodebug)
	return;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

struct infb_conn;

template <typename A, typename B>
inline A round_down(A a, B b) {
    return a / b * b;
}

template <typename A, typename B>
inline A round_up(A a, B b) {
    return (a % b) ? (a / b * b + b) : a;
}

struct infb_sockaddr {
    uint16_t lid_;
    uint32_t qpn_;
    int      gid_index_;
    ibv_gid  gid_;
    uint32_t psn_; // packet sequence number

    void make(uint16_t lid, uint32_t qpn, int gid_index, ibv_gid* gid) {
        lid_ = lid;
        qpn_ = qpn;
	gid_index_ = gid_index;
	if (gid)
	    gid_ = *gid;
	else
            bzero(&gid_, sizeof(gid_));
	srand48(getpid() * time(NULL));
        psn_ = lrand48() & 0xffffff;
    }

    void dump(FILE* fd) const {
	char gidbuf[100];
	inet_ntop(AF_INET6, &gid_, gidbuf, sizeof(gidbuf));
	fprintf(fd, "address; LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
		lid_, qpn_, psn_, gidbuf);
    }
};

struct infb_provider {
    static infb_provider* make(const char* dname, int ib_port, int sl) {
        ibv_device** dl = ibv_get_device_list(NULL);
	CHECK(dl);
	ibv_device* d = NULL;
	if (!dname) {
	    d = dl[0];
	} else {
	    for (int i = 0; dl[i]; ++i)
   	        if (!strcmp(ibv_get_device_name(dl[i]), dname)) {
		    d = dl[i];
		    break;
	        }
	}
	CHECK(d);
	ibv_context* c = ibv_open_device(d);
	CHECK(c);
	return new infb_provider(dl, d, c, ib_port, sl);
    }
    static infb_provider* default_instance() {
	static infb_provider* instance = NULL;
	static std::mutex mu;
	if (!instance) {
	    std::lock_guard<std::mutex> lk(mu);
	    if (!instance)
	        instance = make(NULL, 1, 0);
	}
	return instance;
    }
    ibv_device* device() {
	return d_;
    }
    ibv_context* context() {
	return c_;
    }
    int ib_port() const {
	return ib_port_;
    }
    int sl() {
	return sl_;
    }
    const ibv_device_attr& attr() const {
	return attr_;
    }
    ~infb_provider() {
	stop_ = true;
	t_->join();
	delete t_;
	CHECK(ibv_close_device(c_) == 0);
	ibv_free_device_list(dl_);
    }
  private:
    infb_provider(ibv_device** dl, ibv_device* d, ibv_context* c, int ib_port, int sl) 
	: dl_(dl), d_(d), c_(c), ib_port_(ib_port), sl_(sl) {
	stop_ = false;
	t_ = new std::thread([&]{
		ibv_async_event e;
		while (!stop_) {
		    if (ibv_get_async_event(c_, &e) != 0) {
			perror("ibv_get_async_event");
			exit(1);
		    }
		    fprintf(stderr, "got event %d\n", e.event_type);
		}
	    });
	CHECK(ibv_query_device(c_, &attr_) == 0);
    }
    volatile bool stop_;
    std::thread* t_;
    ibv_device** dl_;
    ibv_device* d_;
    ibv_context* c_;
    int ib_port_;
    int sl_;
    ibv_device_attr attr_;
};

enum infb_conn_type { INFB_CONN_POLL, INFB_CONN_INT, INFB_CONN_ASYNC };

inline infb_conn_type make_infb_type(const char* type) {
    if (strcmp(type, "poll") == 0)
	return INFB_CONN_POLL;
    else if (strcmp(type, "int") == 0)
	return INFB_CONN_INT;
    else if (strcmp(type, "async") == 0)
	return INFB_CONN_ASYNC;
    else {
	fprintf(stderr, "bad infiniband type %s\n", type);
	assert(0);
    }
}

struct infb_conn {
    infb_conn(infb_conn_type type, infb_provider* p) 
	: p_(p), type_(type) {
	pd_ = NULL;
	mr_ = NULL;
	scq_ = rcq_ = NULL;
	qp_ = NULL;
	schan_ = rchan_ = NULL;
	sw_ = NULL;
    }

    bool blocking() const {
	return type_ != INFB_CONN_ASYNC;
    }

    int create() {
	static const int max_inline_data = 400;
	// get port attributes
	CHECK(ibv_query_port(p_->context(), p_->ib_port(), &portattr_) == 0);
	mtu_ = portattr_.active_mtu;
	//mtu_ = IBV_MTU_2048;
	mtub_ = 128 << mtu_;
	// XXX: decide mr size dynamically
	if (type_ != INFB_CONN_POLL) {
	    CHECK(rchan_ = ibv_create_comp_channel(p_->context()));
	    if (blocking()) {
	        CHECK(schan_ = ibv_create_comp_channel(p_->context()));
	    } else
		schan_ = rchan_;
	}
	rcq_ = create_cq(rchan_, 600);
	scq_ = create_cq(schan_, 40);
	if (geteuid() != 0) {
	    fprintf(stderr, "Infiniband requires root priviledge for performance\n");
	    exit(-1);
	}
	printf("max mr size is %ld\n", p_->attr().max_mr_size);
	printf("actual rx_depth %d, tx_depth %d\n", rcq_->cqe, scq_->cqe);

	CHECK(pd_ = ibv_alloc_pd(p_->context()));

	// create and register memory region of size_ bytes
	const int page_size = sysconf(_SC_PAGESIZE);
	buf_.len_ = round_up(mtub_ * (rcq_->cqe + scq_->cqe), page_size);
	printf("allocating %d bytes\n", buf_.len_);
	CHECK(buf_.s_ = (char*)memalign(page_size, buf_.len_));
	bzero(buf_.s_, buf_.len_);
	CHECK(mr_ = ibv_reg_mr(pd_, buf_.s_, buf_.len_, IBV_ACCESS_LOCAL_WRITE));

	// create a RC queue pair
	ibv_qp_init_attr attr;
	bzero(&attr, sizeof(attr));
	attr.send_cq = scq_;
	attr.recv_cq = rcq_;
	attr.cap.max_send_wr = scq_->cqe;
	attr.cap.max_recv_wr = rcq_->cqe;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
	attr.cap.max_inline_data = max_inline_data;
	attr.qp_type = IBV_QPT_RC;
	CHECK(qp_ = ibv_create_qp(pd_, &attr));

	// set the queue pair to init state
	ibv_qp_attr xattr;
	bzero(&xattr, sizeof(xattr));
	xattr.qp_state = IBV_QPS_INIT;
	xattr.pkey_index = 0;
	xattr.port_num = p_->ib_port();
	xattr.qp_access_flags = 0;
	CHECK(ibv_modify_qp(qp_, &xattr, 
			    IBV_QP_STATE 	|
			    IBV_QP_PKEY_INDEX   |
			    IBV_QP_PORT  	| 
			    IBV_QP_ACCESS_FLAGS) == 0);
	for (int i = 0; i < rcq_->cqe; ++i)
	    CHECK(post_recv_with_id(buf_.s_ + mtub_ * i, mtub_) == 0);

	// if the link layer is Ethernet, portattr_.lid is not important;
	// otherwise, we need a valid portattr_.lid.
	CHECK(portattr_.link_layer == IBV_LINK_LAYER_ETHERNET || portattr_.lid);
	// XXX: support user specified gid_index
	local_.make(portattr_.lid, qp_->qp_num, 0, NULL);
	wbuf_.assign(wbuf_start(), scq_->cqe * mtub_);
	nw_ = 0;

	bzero(&attr, sizeof(attr));
	bzero(&xattr, sizeof(xattr));
	CHECK(ibv_query_qp(qp_, &xattr, IBV_QP_CAP, &attr) == 0);
	max_inline_size_ = xattr.cap.max_inline_data;
	printf("max_inline_size: %zd\n", max_inline_size_);
	return 0;
    }

    ~infb_conn() {
	printf("~infb_conn\n");
	if (qp_)
	    CHECK(ibv_destroy_qp(qp_) == 0);
	if (mr_)
	    CHECK(ibv_dereg_mr(mr_) == 0);
	if (pd_)
	    CHECK(ibv_dealloc_pd(pd_) == 0);
	if (scq_)
	    CHECK(ibv_destroy_cq(scq_) == 0);
	if (rcq_)
	    CHECK(ibv_destroy_cq(rcq_) == 0);
	if (buf_.s_)
	    free(buf_.s_);
	if (rchan_)
	    CHECK(ibv_destroy_comp_channel(rchan_) == 0);
	if (schan_ != rchan_)
	    CHECK(ibv_destroy_comp_channel(schan_) == 0);
	if (sw_) {
	    sw_->stop();
	    delete sw_;
	}
    }

    ssize_t read(char* buf, size_t len) {
	assert(len > 0);
	if (!readable()) {
	    if (!blocking()) {
	        errno = EWOULDBLOCK;
	        return -1;
	    }
	    real_read(); // must have made some progress
	    if (!readable()) {
		errno = EIO;
	        return -1;
	    }
	}
	size_t r = 0;
	while (r < len && !pending_read_.empty()) {
	    refcomp::str& rx = pending_read_.front();
	    size_t n = std::min(len - r, rx.length());
	    memcpy(buf + r, rx.data(), n);
	    r += n;
	    if (n == rx.length()) {
		pending_read_.pop();
		post_recv_with_id((char*)round_down(uintptr_t(rx.data()), mtub_), mtub_);
	    } else
		rx.assign(rx.data() + n, rx.length() - n);
	}
	return r;
    }

    ssize_t write(const char* buf, size_t len) {
	assert(len > 0);
	if (!writable(len)) {
	    if (!blocking()) {
	        errno = EWOULDBLOCK;
	        return -1;
	    }
	    real_write(); // must have made some progress
	    if (!writable(len)) {
		errno = EIO;
		return -1;
	    }
	}

	if (len <= max_inline_size_) {
	    if (post_send_with_buffer(buf, len, IBV_SEND_SIGNALED | IBV_SEND_INLINE) == 0)
	        return len;
	    else
		return -1;
	}
	ssize_t r = 0;
	while (r < len && wbuf_.length()) {
	    size_t n = std::min(len - r, wbuf_.length());
	    memcpy(wbuf_.s_, buf + r, n);
	    if (post_send_with_buffer(wbuf_.data(), n, IBV_SEND_SIGNALED) != 0)
		return -1;
	    wbuf_consume(n);
	    r += n;
	}
	
	return r;
    }
    
    const infb_sockaddr& local_address() {
	return local_;
    }

    int connect(const infb_sockaddr& remote) {
	remote_ = remote;
	ibv_qp_attr attr;
	bzero(&attr, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = mtu_;
	attr.dest_qp_num = remote.qpn_;
	attr.rq_psn = remote.psn_; // sequence number
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid = remote.lid_;
	attr.ah_attr.sl = p_->sl();
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = p_->ib_port();
	if (remote.gid_.global.interface_id) {
	    attr.ah_attr.is_global = 1;
	    attr.ah_attr.grh.hop_limit = 1;
	    attr.ah_attr.grh.dgid = remote.gid_;
	    attr.ah_attr.grh.sgid_index = local_.gid_index_;
	}
	if (ibv_modify_qp(qp_, &attr,
			  IBV_QP_STATE			|
			  IBV_QP_AV			|
			  IBV_QP_PATH_MTU		|
			  IBV_QP_DEST_QPN		|
			  IBV_QP_RQ_PSN			|
			  IBV_QP_MAX_DEST_RD_ATOMIC	|
			  IBV_QP_MIN_RNR_TIMER)) {
	    perror("ibv_modify_qp to RTR");
	    return -1;
	}

	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = local_.psn_;
	attr.max_rd_atomic = 1;
	if (ibv_modify_qp(qp_, &attr,
			  IBV_QP_STATE		|
			  IBV_QP_TIMEOUT	|
			  IBV_QP_RETRY_CNT	|
			  IBV_QP_RNR_RETRY	|
			  IBV_QP_SQ_PSN		|
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
	    perror("ibv_modify_qp to RTS");
	    return -1;
	}
	return 0;
    }
    bool read_cq(ibv_cq* cq) const {
	return cq == rcq_;
    }
    bool readable() const {
	return !pending_read_.empty();
    }
    bool writable(size_t len = 0) const {
	return nw_ < scq_->cqe && ((len > 0 && len < max_inline_size_) || wbuf_.length());
    }

    void drain() {
	assert(!blocking() && sw_);
	if (flags_ == 0)
	    return;
   	dbg("drain: dispatch before querying for CQE\n");
	while (true) {
	    int flags = readable() ? ev::READ : 0;
	    flags |= writable() ? ev::WRITE : 0;
	    int interest = flags & flags_;
	    if (!interest)
		break;
	    cb_(this, interest);
	}
    }

    void poll_channel(ev::io& w, int) {
	ibv_cq* cq = wait_channel(schan_);
	if (cq == scq_)
	    real_write();
	else
	    real_read();
    }

    typedef std::function<void(infb_conn*,int)> callback_type;
    void register_loop(ev::loop_ref loop, callback_type cb, int flags) {
	assert(!blocking() && schan_ == rchan_);
	cb_ = cb;
	sw_ = new ev::io(loop);
	sw_->set<infb_conn, &infb_conn::poll_channel>(this);
	rpc::common::sock_helper::make_nonblock(schan_->fd);
	sw_->set(schan_->fd, ev::READ);

	hard_select(flags);
    }
    void eselect(int flags) {
	assert(!blocking());
	if (flags_ == flags)
	    return;
	hard_select(flags);
    }

  private:
    void hard_select(int flags) {
	if (flags)
	    sw_->start();
	else
	    sw_->stop();
	flags_ = flags;
    }

    ibv_cq* wait_channel(ibv_comp_channel* chan) {
        ibv_cq* cq;
	void* ctx;
	if (ibv_get_cq_event(chan, &cq, &ctx)) {
	    perror("ibv_get_cq_event");
	    return NULL;
	}
	CHECK(ctx == this);
	ibv_ack_cq_events(cq, 1);
	CHECK(ibv_req_notify_cq(cq, 0) == 0);
	return cq;
    }

    template <typename F>
    int poll(ibv_cq* cq, F f) {
	ibv_wc wc[cq->cqe];
	int ne;
	do {
	    CHECK((ne = ibv_poll_cq(cq, cq->cqe, wc)) >= 0);
	} while (blocking() && ne < 1);
	for (int i = 0; i < ne; ++i) {
	    if (wc[i].status != IBV_WC_SUCCESS) {
		fprintf(stderr, "poll failed with status: %d\n", wc[i].status);
		return -1;
	    }
	    f(wc[i]);
	}
	return 0;
    }
    int real_read() {
	if (type_ == INFB_CONN_INT)
	    assert(wait_channel(rchan_) == rcq_);
	auto f = [&](const ibv_wc& wc) {
	   	assert(recv_request(wc.wr_id));
		pending_read_.push(refcomp::str((const char*)wc.wr_id, wc.byte_len));
	    };
	return poll(rcq_, f);
    }
    int real_write() {
	if (type_ == INFB_CONN_INT)
	    assert(wait_channel(schan_) == scq_);
	auto f = [&](const ibv_wc& wc) {
	        if (non_inline_send_request(wc.wr_id))
	            wbuf_extend(wc.byte_len);
		--nw_;
	    };
	return poll(scq_, f);
    }

    ibv_cq* create_cq(ibv_comp_channel* chan, int depth) {
	ibv_cq* cq = NULL;
	CHECK(cq = ibv_create_cq(p_->context(), depth, this, chan, 0));
        // when a completion queue entry (CQE) is placed on the CQ,
        // send a completion event to schan_/rchan_ if the channel is empty.
        // mimics the level-trigger file descriptors
        if (chan)
	    CHECK(ibv_req_notify_cq(cq, 0) == 0);
	return cq;
    }

    char* wbuf_start() {
	return buf_.s_ + rcq_->cqe * mtub_;
    }
    char* wbuf_end() {
	return buf_.s_ + (rcq_->cqe + scq_->cqe) * mtub_;
    }
    void wbuf_consume(size_t n) {
	wbuf_.s_ += n;
	if (wbuf_.s_ >= wbuf_end())
	    wbuf_.s_ = wbuf_start() + (wbuf_.s_ - wbuf_end());
	wbuf_.len_ -= mtub_;
    }
    void wbuf_extend(size_t n) {
	wbuf_.len_ += n;
    }

    int post_recv_with_id(char* p, size_t len) {
	assert(len <= mtub_);
	ibv_sge sge;
	make_ibv_sge(sge, p, len, mr_->lkey);

	ibv_recv_wr wr;
	bzero(&wr, sizeof(wr));
	wr.wr_id = uintptr_t(p);
	wr.sg_list = &sge;
	wr.num_sge = 1;

	ibv_recv_wr* bad_wr;
        if (ibv_post_recv(qp_, &wr, &bad_wr)) {
  	    perror("ibv_post_recv");
	    return -1;
	}
	return 0;
    }

    int post_send_with_buffer(const char* buffer, size_t len, int flags) {
	ibv_sge sge;
	make_ibv_sge(sge, buffer, len, mr_->lkey);
	
	ibv_send_wr wr;
	bzero(&wr, sizeof(wr));
	wr.wr_id = uintptr_t(buffer);
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = flags;
	ibv_send_wr* bad_wr;
	if (ibv_post_send(qp_, &wr, &bad_wr)) {
	    perror("ibv_post_send");
	    return -1;
	}
	dbg("post_send_with_buffer: sent %zd bytes\n", len);
	++nw_;
	return 0;
    }

    bool recv_request(uint64_t wrid) {
	return wrid >= uintptr_t(buf_.data()) && 
	       wrid < uintptr_t(wbuf_start());
    }
    bool non_inline_send_request(uint64_t wrid) {
	return wrid >= uintptr_t(wbuf_start()) && 
	       wrid < uintptr_t(wbuf_end());
    }

    static void make_ibv_sge(ibv_sge& list, const char* buffer, size_t size, uint32_t lkey) {
	list.addr = uintptr_t(buffer);
	list.length = size;
	list.lkey = lkey;
    }

    const infb_conn_type type_;
    infb_provider* p_;
    ibv_pd* pd_;
    ibv_mr* mr_;
    ibv_cq* scq_;
    ibv_cq* rcq_;
    ibv_qp* qp_;
    ibv_port_attr portattr_;

    refcomp::str buf_; // the single read/write buffer

    ibv_mtu mtu_;
    size_t mtub_; // mtu in bytes
    size_t max_inline_size_;

    infb_sockaddr local_;
    infb_sockaddr remote_;

    std::queue<refcomp::str> pending_read_; // available read scatter list
    refcomp::str wbuf_; // available write buffer
    int nw_; // number of outstanding writes
    ibv_comp_channel* schan_; // send channel
    ibv_comp_channel* rchan_; // receive channel

    ev::io* sw_; // watcher on the channel (schan_ == rchan_)
    callback_type cb_;
    int flags_;
};

struct infb_server {
    infb_server() : sfd_(-1) {
    }
    void listen(int port) {
        sfd_ = rpc::common::sock_helper::listen(port);
        assert(sfd_ >= 0);
    }
    infb_conn* accept(infb_conn_type type) {
	infb_conn* c = new infb_conn(type, infb_provider::default_instance());
	c->create();
	c->local_address().dump(stdout);

        int fd = rpc::common::sock_helper::accept(sfd_);
        assert(fd >= 0);

        infb_sockaddr remote;
        assert(read(fd, &remote, sizeof(remote)) == sizeof(remote));
	const infb_sockaddr& local = c->local_address();
	assert(write(fd, &local, sizeof(local)) == sizeof(local));
	close(fd);

	remote.dump(stdout);
	assert(c->connect(remote) == 0);
	return c;
    }
  private:
    int sfd_;
};

struct infb_client {
    static infb_conn* connect(const char* ip, int port, infb_conn_type type) {
	infb_conn* c = new infb_conn(type, infb_provider::default_instance());
	c->create();
	c->local_address().dump(stdout);

        int fd = rpc::common::sock_helper::connect(ip, port);
        assert(fd >= 0);
        const infb_sockaddr& local = c->local_address();
        assert(write(fd, &local, sizeof(local)) == sizeof(local));
        infb_sockaddr remote;
        assert(read(fd, &remote, sizeof(remote)) == sizeof(remote));

        remote.dump(stdout);
        assert(c->connect(remote) == 0);

	return c;
    }
};


