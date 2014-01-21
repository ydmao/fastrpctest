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

#define CHECK(x) { if(!(x)){ \
                     fprintf(stderr, "CHECK(%s) failed %s:%d\n", \
                             #x, __FILE__, __LINE__); perror("check:"); exit(1); } }

struct infb_conn;

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

struct infb_conn {
    infb_conn() 
	: dev_(NULL), context_(NULL), channel_(NULL), 
	  buffer_(NULL), w_(NULL), 
	  pending_read_(0), write_room_(0) {
    }
    int create(const char* dname, int ib_port, size_t size, 
	       int rx_depth, bool use_event, int sl, ibv_mtu mtu) {
	ib_port_ = ib_port;
	sl_ = sl;
	mtu_ = mtu;
        ibv_device** dl = ibv_get_device_list(NULL);
	CHECK(dl);
        for (int i = 0; dl[i]; ++i)
   	    if (!dname || !strcmp(ibv_get_device_name(dl[i]), dname)) {
		dev_ = dl[i];
		break;
	    }
	CHECK(dev_);
	CHECK(context_ = ibv_open_device(dev_));
	if (use_event) {
	    CHECK(channel_ = ibv_create_comp_channel(context_));
	}
	CHECK(pd_ = ibv_alloc_pd(context_));

	// create and register memory region of size_ bytes
	const int page_size = sysconf(_SC_PAGESIZE);
        CHECK(size % page_size == 0);
	size_ = size;
	CHECK(buffer_ = (char*)memalign(page_size, size));
	//memset(buffer_, 0, size);
	memset(buffer_, 0x7b, size);
	CHECK(mr_ = ibv_reg_mr(pd_, buffer_, size, IBV_ACCESS_LOCAL_WRITE));

	// create an completion queue with depth of rx_depth
	rx_depth_ = rx_depth;
	CHECK(cq_ = ibv_create_cq(context_, rx_depth + 1, NULL, channel_, 0));

	// create a RC queue pair
	ibv_qp_init_attr attr;
	bzero(&attr, sizeof(attr));
	attr.send_cq = cq_;
	attr.recv_cq = cq_;
	attr.cap.max_send_wr = 1;
	attr.cap.max_recv_wr = rx_depth;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
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

	CHECK(post_recv(rx_depth) == rx_depth);
	rreq_ = rx_depth;

	if (use_event) {
            // when a completion queue entry (CQE) is placed on the CQ,
            // send a completion event to channel_ if the channel_ is empty.
            // mimics the level-trigger file descriptors
	    CHECK(ibv_req_notify_cq(cq_, 0) == 0);
	}

	// get port attributes
	CHECK(ibv_query_port(context_, ib_port, &portattr_) == 0);
	// if the link layer is Ethernet, portattr_.lid is not important;
	// otherwise, we need a valid portattr_.lid.
	CHECK(portattr_.link_layer == IBV_LINK_LAYER_ETHERNET || portattr_.lid);
	// XXX: support user specified gid_index
	local_.make(portattr_.lid, qp_->qp_num, 0, NULL);
	// XXX: how much?
	write_room_ = size_;
	pending_read_ = 0;
	return 0;
    }

    void set_ev_watcher(infb_ev_watcher* w) {
	w_ = w;
    }

    ssize_t read(char* buf, size_t len) {
	//fprintf(stderr, "read one: pending_read %zd, %zd\n", pending_read_, len);
	if (pending_read_ == 0) {
	    errno = EWOULDBLOCK;
	    return -1;
	}
	size_t r = std::min(len, pending_read_);
	memcpy(buf, buffer_, r);
	pending_read_ -= r;

	// XXX: Post all empty read requests
	assert(pending_read_ == 0);
	if (rreq_ == 0) {
  	    post_recv(rx_depth_);
	    rreq_ = rx_depth_;
	}
	return r;
    }

    ssize_t write(const char* buf, size_t len) {
	//fprintf(stderr, "write one: write_room %zd\n", write_room_);
	if (write_room_ == 0) {
	    errno = EWOULDBLOCK;
	    return -1;
	}
	size_t r = std::min(len, write_room_);
	memcpy(buffer_, buf, r);
	write_room_ -= len;

	// XXX: write as many as buffer, and post_send all of them
	assert(write_room_ == 0);
	post_send(1);
	return r;
    }

    void loop_once() {
	if (dispatch_once())
	    return;
	if (channel_) {
	    ibv_cq* ev_cq;
	    void* ev_ctx;
	    CHECK(ibv_get_cq_event(channel_, &ev_cq, &ev_ctx) == 0);
	    CHECK(ev_cq == cq_ && ev_ctx == (void*)context_);
	    CHECK(ibv_req_notify_cq(cq_, 0));
	}
	ibv_wc wc[2];
	int ne;
	do {
	    CHECK((ne = ibv_poll_cq(cq_, 2, wc)) >= 0);
	} while (!channel_ && ne < 1);
	for (int i = 0; i < ne; ++i) {
	    CHECK(wc[i].status == IBV_WC_SUCCESS);
	    if (wc[i].wr_id == receive_work_request_id) {
		pending_read_ += size_;
		--rreq_;
		//fprintf(stderr, "receive_work_done: %zd\n", pending_read_);
	    } else if (wc[i].wr_id == send_work_request_id) {
		write_room_ += size_;
		//fprintf(stderr, "send_work_done: %zd\n", write_room_);
	    } else {
		assert(0 && "Bad wr_id");
	    }
	}
	dispatch_once();
    }
    
    bool dispatch_once() {
	int flags = 0;
	if (pending_read_ > 0)
	    flags |= INFB_EV_READ;
	if (write_room_ > 0)
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
    enum { receive_work_request_id = 1, send_work_request_id = 2 };

    int post_recv(int n) {
	ibv_sge sge;
	make_ibv_sge(sge, buffer_, size_, mr_->lkey);

	ibv_recv_wr wr;
	bzero(&wr, sizeof(wr));
	wr.wr_id = receive_work_request_id;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	ibv_recv_wr* bad_wr;
	for (int i = 0; i < n; ++i)
	    if (ibv_post_recv(qp_, &wr, &bad_wr)) {
		perror("ibv_post_recv");
		return i;
	    }
	return n;
    }

    int post_send(int n) {
	ibv_sge sge;
	make_ibv_sge(sge, buffer_, size_, mr_->lkey);
	
	ibv_send_wr wr;
	bzero(&wr, sizeof(wr));
	wr.wr_id = send_work_request_id;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	
	ibv_send_wr* bad_wr;
	for (int i = 0; i < n; ++i)
	    if (ibv_post_send(qp_, &wr, &bad_wr)) {
		perror("ibv_post_send");
		return i;
	    }
	return n;
    }

    static void make_ibv_sge(ibv_sge& list, char* buffer, size_t size, uint32_t lkey) {
	list.addr = uintptr_t(buffer);
	list.length = size;
	list.lkey = lkey;
    }

    ibv_device* dev_;
    ibv_context* context_;
    ibv_comp_channel* channel_;
    ibv_pd* pd_;
    ibv_mr* mr_;
    ibv_cq* cq_;
    ibv_qp* qp_;
    ibv_port_attr portattr_;

    size_t size_;
    char* buffer_;
    int rx_depth_;
    int ib_port_;
    int sl_;
    ibv_mtu mtu_;

    infb_sockaddr local_;
    infb_sockaddr remote_;

    infb_ev_watcher* w_;

    int rreq_; // number of outstanding read request
    size_t pending_read_; // number of bytes pending
    size_t write_room_;   // number of bytes avaiable in the write buffer
};


