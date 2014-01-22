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

#include "compiler/str.hh"

#define CHECK(x) { if(!(x)){ \
                     fprintf(stderr, "CHECK(%s) failed %s:%d\n", \
                             #x, __FILE__, __LINE__); perror("check:"); exit(1); } }

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

    infb_ev_watcher() : flags_(0) {}
    virtual ~infb_ev_watcher() {}

    void start(int flags) {
	assert(flags);
	flags_ = flags;
    }
    void stop() {
	flags_ = 0;
    }
    bool operator()(infb_conn* c, int flags) {
	int interest = flags & flags_;
	if (cb_ && interest)
	    cb_(this, interest, c);
	return interest;
    }
    void set(cb_type cb) {
	cb_ = cb;
    }
  private:
    int flags_;
    cb_type cb_;
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

struct infb_provider {
    static infb_provider* make(const char* dname = NULL) {
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
	return new infb_provider(d, c);
    }
    ibv_device* device() {
	return d_;
    }
    ibv_context* context() {
	return c_;
    }
  private:
    infb_provider(ibv_device* d, ibv_context* c) : d_(d), c_(c) {
    }
    ibv_device* d_;
    ibv_context* c_;
};

struct infb_conn {
    infb_conn() : provider_(NULL), channel_(NULL), w_(NULL) {
    }
    
    int create(infb_provider* provider, int ib_port, bool use_event, int sl) {
	static const int max_inline_data = 400;
	provider_ = provider;
	// get port attributes
	CHECK(ibv_query_port(provider->context(), ib_port, &portattr_) == 0);
	//mtu_ = portattr_.active_mtu;
	mtu_ = IBV_MTU_4096;
	mtub_ = 128 << mtu_;
	ib_port_ = ib_port;
	sl_ = sl;
        tx_depth_ = 1;
	rx_depth_ = 10;

	if (use_event) {
	    CHECK(channel_ = ibv_create_comp_channel(provider->context()));
	}
	CHECK(pd_ = ibv_alloc_pd(provider->context()));

	// create and register memory region of size_ bytes
	const int page_size = sysconf(_SC_PAGESIZE);
	buf_.len_ = round_up(mtub_ * (rx_depth_ + tx_depth_), page_size);
	printf("allocating %d bytes\n", buf_.len_);
	CHECK(buf_.s_ = (char*)memalign(page_size, buf_.len_));
	bzero(buf_.s_, buf_.len_);
	CHECK(mr_ = ibv_reg_mr(pd_, buf_.s_, buf_.len_, IBV_ACCESS_LOCAL_WRITE));

	// create an completion queue with depth of rx_depth
	CHECK(cq_ = ibv_create_cq(provider->context(), rx_depth_ + tx_depth_, NULL, channel_, 0));

	// create a RC queue pair
	ibv_qp_init_attr attr;
	bzero(&attr, sizeof(attr));
	attr.send_cq = cq_;
	attr.recv_cq = cq_;
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
	xattr.port_num = ib_port;
	xattr.qp_access_flags = 0;
	CHECK(ibv_modify_qp(qp_, &xattr, 
			    IBV_QP_STATE 	|
			    IBV_QP_PKEY_INDEX   |
			    IBV_QP_PORT  	| 
			    IBV_QP_ACCESS_FLAGS) == 0);
	for (int i = 0; i < rx_depth_; ++i)
	    CHECK(post_recv_with_id(buf_.s_ + mtub_ * i, mtub_) == 0);

	if (use_event) {
            // when a completion queue entry (CQE) is placed on the CQ,
            // send a completion event to channel_ if the channel_ is empty.
            // mimics the level-trigger file descriptors
	    CHECK(ibv_req_notify_cq(cq_, 0) == 0);
	}

	// if the link layer is Ethernet, portattr_.lid is not important;
	// otherwise, we need a valid portattr_.lid.
	CHECK(portattr_.link_layer == IBV_LINK_LAYER_ETHERNET || portattr_.lid);
	// XXX: support user specified gid_index
	local_.make(portattr_.lid, qp_->qp_num, 0, NULL);
	wbuf_.assign(wbuf_start(), tx_depth_ * mtub_);

	bzero(&attr, sizeof(attr));
	bzero(&xattr, sizeof(xattr));
	CHECK(ibv_query_qp(qp_, &xattr, IBV_QP_CAP, &attr) == 0);
	max_inline_size_ = xattr.cap.max_inline_data;
	printf("max_inline_size: %zd\n", max_inline_size_);
	return 0;
    }

    void set_ev_watcher(infb_ev_watcher* w) {
	w_ = w;
    }

    ssize_t read(char* buf, size_t len) {
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

    void loop_once() {
	if (dispatch_once())
	    return;
	if (channel_) {
	    ibv_cq* cq;
	    void* ctx;
	    CHECK(ibv_get_cq_event(channel_, &cq, &ctx) == 0);
	    CHECK(cq == cq_);
	    // ctx is provided by user on ibv_create_cq
	    CHECK(ctx == NULL);
	    CHECK(ibv_req_notify_cq(cq_, 0) == 0);
	}
	ibv_wc wc[rx_depth_ + tx_depth_];
	int ne;
	do {
	    CHECK((ne = ibv_poll_cq(cq_, rx_depth_ + tx_depth_, wc)) >= 0);
	} while (!channel_ && ne < 1);
	for (int i = 0; i < ne; ++i) {
	    CHECK(wc[i].status == IBV_WC_SUCCESS);
	    if (recv_request(wc[i].wr_id))
		pending_read_.push(refcomp::str((const char*)wc[i].wr_id, wc[i].byte_len));
	    else if (non_inline_send_request(wc[i].wr_id))
		wbuf_extend(uintptr_t(wc[i].wr_id));
	}
	dispatch_once();
    }
    
    bool dispatch_once() {
	int flags = 0;
	if (pending_read_.empty() == false)
	    flags |= INFB_EV_READ;
	if (wbuf_.length() > 0)
	    flags |= INFB_EV_WRITE;
	if (flags && w_)
	    return w_->operator()(this, flags);
	return false;
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
	attr.ah_attr.sl = sl_;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = ib_port_;
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

  private:
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

    infb_provider* provider_;
    ibv_comp_channel* channel_;
    ibv_pd* pd_;
    ibv_mr* mr_;
    ibv_cq* cq_;
    ibv_qp* qp_;
    ibv_port_attr portattr_;

    refcomp::str buf_; // the single read/write buffer

    int rx_depth_;
    int tx_depth_;
    int ib_port_;
    int sl_;
    ibv_mtu mtu_;
    size_t mtub_; // mtu in bytes
    size_t max_inline_size_;

    infb_sockaddr local_;
    infb_sockaddr remote_;

    infb_ev_watcher* w_;

    std::queue<refcomp::str> pending_read_; // available read scatter list
    refcomp::str wbuf_; // available write buffer
};
