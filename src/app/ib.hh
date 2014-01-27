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

#include "rpc_common/sock_helper.hh"
#include "compiler/str.hh"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define CHECK(x) { if(!(x)){ \
                     fprintf(stderr, "CHECK(%s) failed %s:%d\n", \
                             #x, __FILE__, __LINE__); perror("check"); exit(1); } }

struct infb_conn;

template <typename A, typename B>
inline A round_down(A a, B b) {
    return a / b * b;
}

template <typename A, typename B>
inline A round_up(A a, B b) {
    return (a % b) ? (a / b * b + b) : a;
}

enum { INFB_EV_READ = 0x1, INFB_EV_WRITE = 0x2 };

struct infb_ev_watcher {
    typedef void (*cb_type)(infb_ev_watcher* w, int revents, infb_conn* c);

    infb_ev_watcher(infb_conn* c) : flags_(0), c_(c) {}
    virtual ~infb_ev_watcher() {}

    void set(int flags) {
	assert(flags);
	flags_ = flags;
    }
    bool operator()(int flags) {
	int interest = flags & flags_;
	if (cb_ && interest)
	    cb_(this, interest, c_);
	return interest;
    }
    void set(cb_type cb) {
	cb_ = cb;
    }
    infb_conn* conn() {
	return c_;
    }
  protected:
    friend struct infb_loop;
    infb_ev_watcher() : flags_(0), c_(NULL) {}

    int flags_;
    cb_type cb_;
    infb_conn* c_;
};

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

struct infb_provider;

struct infb_conn_factory {
    infb_conn_factory(infb_provider* p) : p_(p) {}
    virtual ~infb_conn_factory() {}
    virtual infb_conn* make_conn() = 0;
  protected:
    infb_provider* p_;
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
	return new infb_provider(d, c, ib_port, sl);
    }
    static infb_provider* default_instance() {
	static infb_provider* instance = NULL;
	if (!instance)
	    instance = make(NULL, 1, 0);
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
  private:
    infb_provider(ibv_device* d, ibv_context* c, int ib_port, int sl) 
	: d_(d), c_(c), ib_port_(ib_port), sl_(sl) {
    }
    ibv_device* d_;
    ibv_context* c_;
    int ib_port_;
    int sl_;
};

// blocking poll socket
struct infb_poll_factory : public infb_conn_factory {
    infb_conn* make_conn();
    infb_poll_factory(infb_provider* p) : infb_conn_factory(p) {
    }
};

// blocking interrupt connection
struct infb_interrupt_factory : public infb_conn_factory {
    infb_conn* make_conn();

    infb_interrupt_factory(infb_provider* p) : infb_conn_factory(p) {
    }
};

struct infb_conn {
    infb_conn(bool blocking, infb_provider* p, ibv_comp_channel* schan, 
	      ibv_comp_channel* rchan, void* cqctx) 
	: p_(p), blocking_(blocking), cqctx_(cqctx), schan_(schan), rchan_(rchan) {
    }
    infb_conn(bool blocking, infb_provider* p, ibv_comp_channel* schan,
	      ibv_comp_channel* rchan) : infb_conn(blocking, p, schan, rchan, this) {
    }

    ibv_cq* create_cq(ibv_comp_channel* channel, int depth) {
	ibv_cq* cq = NULL;
	CHECK(cq = ibv_create_cq(p_->context(), depth, cqctx_, channel, 0));
        if (channel) {
            // when a completion queue entry (CQE) is placed on the CQ,
            // send a completion event to schan_/rchan_ if the channel is empty.
            // mimics the level-trigger file descriptors
	    CHECK(ibv_req_notify_cq(cq, 0) == 0);
	}
	return cq;
    }

    int create() {
	static const int max_inline_data = 400;
	// get port attributes
	CHECK(ibv_query_port(p_->context(), p_->ib_port(), &portattr_) == 0);
	//mtu_ = portattr_.active_mtu;
	mtu_ = IBV_MTU_4096;
	mtub_ = 128 << mtu_;
        tx_depth_ = 1;
	rx_depth_ = 9;

	CHECK(pd_ = ibv_alloc_pd(p_->context()));

	// create and register memory region of size_ bytes
	const int page_size = sysconf(_SC_PAGESIZE);
	buf_.len_ = round_up(mtub_ * (rx_depth_ + tx_depth_), page_size);
	printf("allocating %d bytes\n", buf_.len_);
	CHECK(buf_.s_ = (char*)memalign(page_size, buf_.len_));
	bzero(buf_.s_, buf_.len_);
	CHECK(mr_ = ibv_reg_mr(pd_, buf_.s_, buf_.len_, IBV_ACCESS_LOCAL_WRITE));
	
	rcq_ = create_cq(rchan_, rx_depth_);
	scq_ = create_cq(schan_, tx_depth_);

	// create a RC queue pair
	ibv_qp_init_attr attr;
	bzero(&attr, sizeof(attr));
	attr.send_cq = scq_;
	attr.recv_cq = rcq_;
	attr.cap.max_send_wr = tx_depth_;
	attr.cap.max_recv_wr = rx_depth_;
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
	for (int i = 0; i < rx_depth_; ++i)
	    CHECK(post_recv_with_id(buf_.s_ + mtub_ * i, mtub_) == 0);

	// if the link layer is Ethernet, portattr_.lid is not important;
	// otherwise, we need a valid portattr_.lid.
	CHECK(portattr_.link_layer == IBV_LINK_LAYER_ETHERNET || portattr_.lid);
	// XXX: support user specified gid_index
	local_.make(portattr_.lid, qp_->qp_num, 0, NULL);
	wbuf_.assign(wbuf_start(), tx_depth_ * mtub_);
	nw_ = 0;

	bzero(&attr, sizeof(attr));
	bzero(&xattr, sizeof(xattr));
	CHECK(ibv_query_qp(qp_, &xattr, IBV_QP_CAP, &attr) == 0);
	max_inline_size_ = xattr.cap.max_inline_data;
	printf("max_inline_size: %zd\n", max_inline_size_);
	return 0;
    }
    ssize_t read(char* buf, size_t len) {
	if (pending_read_.empty())
	    real_read();
	if (pending_read_.empty()) {
	    errno = EWOULDBLOCK;
	    return -1;
	}
	assert(len > 0);
	size_t r = 0;
	while (r < len && !pending_read_.empty()) {
	    refcomp::str& rx = pending_read_.front();
	    size_t n = std::min(len - r, rx.length());
	    memcpy(buf + r, rx.data(), n);
	    r += n;
	    if (n == rx.length()) {
		pending_read_.pop();
		post_recv_with_id((char*)round_down(uintptr_t(rx.s_), mtub_), mtub_);
	    } else
		rx.assign(rx.data() + n, rx.length() - n);
	}
	return r;
    }

    ssize_t write(const char* buf, size_t len) {
	if (nw_ == tx_depth_)
	    real_write();

	if (wbuf_.length() == 0) {
	    errno = EWOULDBLOCK;
	    return -1;
	}
	if (len <= max_inline_size_ && wbuf_.length() == tx_depth_ * mtub_) {
	    if (post_send_with_buffer(buf, len, IBV_SEND_SIGNALED | IBV_SEND_INLINE) == 0)
	        return len;
	    else
		return -1;
	}
	assert(wbuf_.length() % mtub_ == 0);
	ssize_t r = 0;
	while (r < len && wbuf_.length()) {
	    size_t n = std::min(len - r, mtub_);
	    memcpy(wbuf_.s_, buf + r, n);
	    if (post_send_with_buffer(wbuf_.data(), n, IBV_SEND_SIGNALED) != 0)
		return -1;
	    wbuf_consume();
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
    bool writable() const {
	return nw_ != tx_depth_;
    }
    void* cqctx() {
	return cqctx_;
    }

  private:
    void wait_channel(ibv_comp_channel* chan, ibv_cq* cq, int depth) {
	if (!chan)
	    return;
        ibv_cq* xcq;
	void* ctx;
	CHECK(ibv_get_cq_event(chan, &xcq, &ctx) == 0);
	CHECK(xcq == cq);
	CHECK(ctx == cqctx_);
	ibv_ack_cq_events(cq, 1);
	CHECK(ibv_req_notify_cq(cq, 0) == 0);
    }

    template <typename F>
    int poll(ibv_cq* cq, int depth, F f) {
	ibv_wc wc[depth];
	int ne;
	do {
	    CHECK((ne = ibv_poll_cq(cq, depth, wc)) >= 0);
	} while (blocking_ && ne < 1);
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
	if (blocking_)
	    wait_channel(rchan_, rcq_, rx_depth_);
	auto f = [&](const ibv_wc& wc) {
	   	assert(recv_request(wc.wr_id));
		pending_read_.push(refcomp::str((const char*)wc.wr_id, wc.byte_len));
	    };
	return poll(rcq_, rx_depth_, f);
    }
    int real_write() {
	if (blocking_)
	    wait_channel(schan_, scq_, tx_depth_);
	auto f = [&](const ibv_wc& wc) {
	        if (non_inline_send_request(wc.wr_id))
	            wbuf_extend(uintptr_t(wc.wr_id));
		--nw_;
	    };
	return poll(scq_, tx_depth_, f);
    }

    char* wbuf_start() {
	return buf_.s_ + rx_depth_ * mtub_;
    }
    char* wbuf_end() {
	return buf_.s_ + (rx_depth_ + tx_depth_) * mtub_;
    }
    void wbuf_consume() {
	wbuf_.s_ += mtub_;
	if (wbuf_.s_ == wbuf_end())
	    wbuf_.s_ = wbuf_start();
	wbuf_.len_ -= mtub_;
    }
    void wbuf_extend(uint64_t wrid) {
	wbuf_.len_ += mtub_;
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
	fprintf(stderr, "written\n");
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

    infb_provider* p_;
    ibv_pd* pd_;
    ibv_mr* mr_;
    ibv_cq* scq_;
    ibv_cq* rcq_;
    ibv_qp* qp_;
    ibv_port_attr portattr_;

    refcomp::str buf_; // the single read/write buffer

    int rx_depth_;
    int tx_depth_;
    ibv_mtu mtu_;
    size_t mtub_; // mtu in bytes
    size_t max_inline_size_;

    infb_sockaddr local_;
    infb_sockaddr remote_;

    bool blocking_;

    std::queue<refcomp::str> pending_read_; // available read scatter list
    refcomp::str wbuf_; // available write buffer
    int nw_; // number of outstanding writes
    void* cqctx_;
    ibv_comp_channel* schan_;
    ibv_comp_channel* rchan_;
};

struct infb_loop : public infb_conn_factory {
    static infb_loop* make(infb_provider* p) {
	ibv_comp_channel* c = NULL;
	CHECK(c = ibv_create_comp_channel(p->context()));
        return new infb_loop(p, c);
    }
    infb_conn* make_conn() {
	infb_ev_watcher* w = new infb_ev_watcher();
	infb_conn* c = new infb_conn(false, p_, c_, c_, w);
	c->create();
	w->c_ = c;
	wch_.push_back(w);
	return c;
    }
    infb_ev_watcher* ev_watcher(infb_conn* c) {
	void* cqctx = c->cqctx();
	assert(cqctx != c);
	return reinterpret_cast<infb_ev_watcher*>(cqctx);
    }
    void destroy(infb_ev_watcher* w) {
	for (auto it = wch_.begin(); it != wch_.end(); it++)
	    if (*it == w) {
		deletion_ = true;
		wch_.erase(it);
		return;
	    }
	assert(0);
    }

    void loop_once() {
	deletion_ = false;
	for (int i = 0; i < (int)wch_.size(); ++i) {
	    int flags = wch_[i]->conn()->readable() ? INFB_EV_READ : 0;
	    flags |= wch_[i]->conn()->writable() ? INFB_EV_WRITE : 0;
	    wch_[i]->operator()(flags);
	    if (unlikely(deletion_)) {
		deletion_ = false;
		--i;
	    }
	}
	if (!wch_.size())
	    return;
	ibv_cq* cq;
	void* ctx;
	CHECK(ibv_get_cq_event(c_, &cq, &ctx) == 0);
	CHECK(ctx != NULL);
	ibv_ack_cq_events(cq, 1);
	CHECK(ibv_req_notify_cq(cq, 0) == 0);

	infb_ev_watcher* w = reinterpret_cast<infb_ev_watcher*>(ctx);
	infb_conn* c = w->conn();
	int flags = c->readable() ? INFB_EV_READ : 0;
	flags |= c->writable() ? INFB_EV_WRITE : 0;
	w->operator()(flags | (w->conn()->read_cq(cq) ? INFB_EV_READ : INFB_EV_WRITE));
    }
    infb_provider* provider() {
	return p_;
    }
  private:
    infb_loop(infb_provider* p, ibv_comp_channel* c) : infb_conn_factory(p), c_(c) {
    }
    ibv_comp_channel* c_;
    std::vector<infb_ev_watcher*> wch_;
    bool deletion_;
};

struct infb_server {
    infb_server() : sfd_(-1) {
    }
    void listen(int port) {
        sfd_ = rpc::common::sock_helper::listen(port);
        assert(sfd_ >= 0);
    }
    infb_conn* accept(infb_conn_factory* f) {
	infb_conn* c = f->make_conn();
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
    static infb_conn* connect(const char* ip, int port, infb_conn_factory* f) {
	infb_conn* c = f->make_conn();
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

infb_conn* infb_poll_factory::make_conn() {
    infb_conn* c = new infb_conn(true, p_, NULL, NULL);
    c->create();
    return c;
}

infb_conn* infb_interrupt_factory::make_conn() {
    ibv_comp_channel* rchan;
    ibv_comp_channel* schan;
    CHECK(rchan = ibv_create_comp_channel(p_->context()));
    CHECK(schan = ibv_create_comp_channel(p_->context()));
    infb_conn* c = new infb_conn(true, p_, schan, rchan);
    c->create();
    return c;
}
