/*
 * Copyright (c) 2013,2014 Minoru NAKAMURA <nminoru@nminoru.jp>
 *
 * This code is licenced under the GPL version 2 or BSD license.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/bitmap.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/rtnetlink.h>
#include <linux/if_vlan.h>
#include <linux/random.h>
#include <linux/kthread.h>
#include <net/sock.h> /* for struct sock */

#include <rdma/ib_user_verbs.h>
#include <rdma/ib_pack.h>

#include "pib.h"
#include "pib_packet.h"


static int kthread_routine(void *data);
static int create_socket(struct pib_dev *dev, int port_index);
static void release_socket(struct pib_dev *dev, int port_index);
static void process_on_qp_scheduler(struct pib_dev *dev);
static int process_new_send_wr(struct pib_qp *qp);
static int process_send_wr(struct pib_dev *dev, struct pib_qp *qp, struct pib_send_wqe *send_wqe);
static int process_incoming_message(struct pib_dev *dev, int port_index);
static void process_incoming_message_per_qp(struct pib_dev *dev, int port_index, u16 dlid, u32 dest_qp_num, struct pib_packet_lrh *lrh, struct ib_grh *grh, struct pib_packet_bth *bth, void *buffer, int size);
static void process_on_wq_scheduler(struct pib_dev *dev);
static void process_sendmsg(struct pib_dev *dev);
static struct sockaddr *get_sockaddr_from_dlid(struct pib_dev *dev, u8 port_num, u32 src_qp_num, u16 dlid);
static void sock_data_ready_callback(struct sock *sk, int bytes);
static void timer_timeout_callback(unsigned long opaque);


static int send_buffer_size;
module_param_named(send_buffer_size, send_buffer_size, int, S_IRUGO);
MODULE_PARM_DESC(send_buffer_size, "Bytes of send buffer");

static int recv_buffer_size;
module_param_named(recv_buffer_size, recv_buffer_size, int, S_IRUGO);
MODULE_PARM_DESC(recv_buffer_size, "Bytes of recv buffer");


int pib_create_kthread(struct pib_dev *dev)
{
	int i, j, ret;
	struct task_struct *task;

	init_completion(&dev->thread.completion);
	init_timer(&dev->thread.timer);

	dev->thread.timer.function = timer_timeout_callback;
	dev->thread.timer.data     = (unsigned long)dev;

	dev->thread.buffer	   = vmalloc(PIB_PACKET_BUFFER);
	if (!dev->thread.buffer) {
		ret = -ENOMEM;
		goto err_vmalloc;
	}

	for (i=0 ; i < dev->ib_dev.phys_port_cnt ; i++) {
		ret = create_socket(dev, i);
		if (ret < 0)
			goto err_sock;
	}

	task = kthread_create(kthread_routine, dev, "pib_%d", dev->dev_id);
	if (IS_ERR(task))
		goto err_task;

	dev->thread.task = task;

	wake_up_process(task);

	return 0;

err_task:
	ret = PTR_ERR(task);
	
err_sock:
	for (j = i-1 ; 0 <= j ; j--)
		release_socket(dev, j);

	vfree(dev->thread.buffer);

err_vmalloc:

	return ret;
}


void pib_release_kthread(struct pib_dev *dev)
{
	int i;

	smp_wmb();

	del_timer_sync(&dev->thread.timer);

	if (dev->thread.task) {
		set_bit(PIB_THREAD_STOP, &dev->thread.flags);
		complete(&dev->thread.completion);
		/* flush_kthread_worker(worker); */
		kthread_stop(dev->thread.task);
		dev->thread.task = NULL;
	}

	for (i=dev->ib_dev.phys_port_cnt - 1 ; 0 <= i  ; i--)
		release_socket(dev, i);

	vfree(dev->thread.buffer);
}


static int create_socket(struct pib_dev *dev, int port_index)
{
	int ret, addrlen;
	int rcvbuf_size, sndbuf_size;
	struct socket *socket;
	struct sockaddr_in sockaddr_in;
	struct sockaddr_in *sockaddr_in_p;

	ret = sock_create_kern(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &socket);
	if (ret < 0)
		return ret;

	/* sk_change_net(sock->sk, net); */

	lock_sock(socket->sk);
	socket->sk->sk_user_data  = dev;
	socket->sk->sk_data_ready = sock_data_ready_callback;

	socket->sk->sk_userlocks |= (SOCK_RCVBUF_LOCK | SOCK_SNDBUF_LOCK);

	sndbuf_size = max_t(u32, send_buffer_size, SOCK_MIN_SNDBUF);
	rcvbuf_size = max_t(u32, recv_buffer_size, SOCK_MIN_RCVBUF);
	socket->sk->sk_sndbuf     = max_t(u32, socket->sk->sk_sndbuf, sndbuf_size);
	socket->sk->sk_rcvbuf     = max_t(u32, socket->sk->sk_rcvbuf, rcvbuf_size);

	release_sock(socket->sk);

	memset(&sockaddr_in, 0, sizeof(sockaddr_in));

	sockaddr_in.sin_family      = AF_INET;
	sockaddr_in.sin_addr.s_addr = htonl(INADDR_ANY);

	ret = kernel_bind(socket, (struct sockaddr *)&sockaddr_in, sizeof(sockaddr_in));
	if (ret < 0) {
		pr_err("pib: kernel_bind: ret=%d\n", ret);
		goto err_sock;
	}

	/* get the port number that is automatically allocated by kernel_bind() */

	addrlen = sizeof(sockaddr_in);
	ret = kernel_getsockname(socket,(struct sockaddr *)&sockaddr_in, &addrlen);
	if (ret < 0) {
		pr_err("pib: kernel_getsockname: ret=%d\n", ret);
		goto err_sock;
	}

#if 0
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
	pib_debug("kernel_getsockname: %pISpc\n",
		  (const struct sockaddr*)&sockaddr_in);
#else
	pib_debug("kernel_getsockname: %08x:%u\n",
		  ntohl(sockaddr_in.sin_addr.s_addr),
		  ntohs(sockaddr_in.sin_port));
#endif
#endif

	dev->ports[port_index].socket = socket;

	/* register lid_table */
	sockaddr_in_p  = kzalloc(sizeof(struct sockaddr_in), GFP_KERNEL);

	sockaddr_in_p->sin_family	= AF_INET;
	sockaddr_in_p->sin_addr.s_addr	= htonl(INADDR_LOOPBACK);
	sockaddr_in_p->sin_port		= sockaddr_in.sin_port;

	dev->ports[port_index].sockaddr	= (struct sockaddr *)sockaddr_in_p;

	return 0;

err_sock:
	sock_release(socket);

	return ret;
}


static void release_socket(struct pib_dev *dev, int port_index)
{
#ifndef PIB_USE_EASY_SWITCH
	int i;
#endif

	if (dev->ports[port_index].sockaddr) {
		kfree(dev->ports[port_index].sockaddr);
		dev->ports[port_index].sockaddr = NULL;
	}

#ifndef PIB_USE_EASY_SWITCH
	for (i=0 ; i<PIB_MAX_LID ; i++) {
		if (dev->ports[port_index].lid_table[i]) {
			kfree(dev->ports[port_index].lid_table[i]);
			dev->ports[port_index].lid_table[i] = NULL;
		}
	}
#endif

	if (dev->ports[port_index].socket) {
		sock_release(dev->ports[port_index].socket);
		dev->ports[port_index].socket = NULL;
	}
}


static int kthread_routine(void *data)
{
	struct pib_dev *dev;
	
	dev = (struct pib_dev *)data;

	BUG_ON(!dev);

#if 0
	/* Hibernation / freezing of the SRPT kernel thread is not supported. */
	current->flags |= PF_NOFREEZE;
#endif

	while (!kthread_should_stop()) {
		unsigned long flags;
		unsigned long timeout = HZ;

		/* 停止時間を計算。ただし1 秒以上は停止させない */
		spin_lock_irqsave(&dev->qp_sched.lock, flags);
		if (time_after(dev->qp_sched.wakeup_time, jiffies))
			timeout = dev->qp_sched.wakeup_time - jiffies;
		else
			dev->qp_sched.wakeup_time = jiffies;
		if (HZ < timeout)
			timeout = HZ;
		spin_unlock_irqrestore(&dev->qp_sched.lock, flags);

		wait_for_completion_interruptible_timeout(&dev->thread.completion, timeout);
		init_completion(&dev->thread.completion);

		while (dev->thread.flags) {
			cond_resched();

			if (test_and_clear_bit(PIB_THREAD_STOP, &dev->thread.flags))
				goto done;

			if (test_and_clear_bit(PIB_THREAD_READY_TO_RECV, &dev->thread.flags)) {
				int i, ret;
				for (i=0 ; i < dev->ib_dev.phys_port_cnt ; i++) {
					do {
						ret = process_incoming_message(dev, i);
					} while (ret == 0);
				}
			}

			if (test_and_clear_bit(PIB_THREAD_WQ_SCHEDULE, &dev->thread.flags))
				process_on_wq_scheduler(dev);

			if (test_and_clear_bit(PIB_THREAD_QP_SCHEDULE, &dev->thread.flags))
				process_on_qp_scheduler(dev);
		}

		process_on_qp_scheduler(dev);
	}

done:
	return 0;
}


static void process_on_qp_scheduler(struct pib_dev *dev)
{
	int ret;
	unsigned long now;
	unsigned long flags;
	struct pib_qp *qp;
	struct pib_send_wqe *send_wqe, *next_send_wqe;

restart:
	now = jiffies;

	spin_lock_irqsave(&dev->lock, flags);

	qp = pib_util_get_first_scheduling_qp(dev);
	if (!qp) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return;
	}

	/* @notice ロックの入れ子関係を一部崩している */
	spin_lock(&qp->lock);
	spin_unlock(&dev->lock);

	/* Responder: generating acknowledge packets */
	if (qp->qp_type == IB_QPT_RC)
		if (pib_generate_rc_qp_acknowledge(dev, qp) == 1)
			goto done;

	/* Requester: generating request packets */
	if ((qp->state != IB_QPS_RTS) && (qp->state != IB_QPS_SQD))
		goto done;

	/*
	 *  Waiting listE の先頭の Send WQE が再送時刻に達していれば
	 *  waiting list から sending list へ戻して再送信を促す。
	 */
	if (list_empty(&qp->requester.waiting_swqe_head))
		goto first_sending_wsqe;

	send_wqe = list_first_entry(&qp->requester.waiting_swqe_head, struct pib_send_wqe, list);

	if (time_after(send_wqe->processing.local_ack_time, now))
		goto first_sending_wsqe;

	send_wqe->processing.retry_cnt--;
	send_wqe->processing.local_ack_time = now + PIB_SCHED_TIMEOUT;

	/* waiting list から sending list へ戻す */
	list_for_each_entry_safe_reverse(send_wqe, next_send_wqe, &qp->requester.waiting_swqe_head, list) {
		send_wqe->processing.list_type = PIB_SWQE_SENDING;
		list_del_init(&send_wqe->list);
		list_add(&send_wqe->list, &qp->requester.sending_swqe_head);
		qp->requester.nr_waiting_swqe--;
		qp->requester.nr_sending_swqe++;
	}

	/* 送信したパケット数をキャンセルする */
	list_for_each_entry(send_wqe, &qp->requester.sending_swqe_head, list) {
		send_wqe->processing.sent_packets = send_wqe->processing.ack_packets;
	}
	    
first_sending_wsqe:
	if (list_empty(&qp->requester.sending_swqe_head)) {
		/* sending list が空になったら新しい SWQE を取り出す */
		if (process_new_send_wr(qp))
			goto first_sending_wsqe;
		else
			goto done;
	}

	send_wqe = list_first_entry(&qp->requester.sending_swqe_head, struct pib_send_wqe, list);

	/*
	 *  Sending list の先頭の Send WQE がエラーだが、waiting list が
	 *  残っている場合、waiting list から空になるまで送信は再開しない。
	 */
	if (send_wqe->processing.status != IB_WC_SUCCESS)
		if (!list_empty(&qp->requester.waiting_swqe_head))
			goto done;

	/*
	 *  RNR NAK タイムアウト時刻の判定
	 */
	if (time_after(send_wqe->processing.schedule_time, now))
		goto done;

	send_wqe->processing.schedule_time = now;

	ret = process_send_wr(dev, qp, send_wqe);
			
	switch (send_wqe->processing.list_type) {

	case PIB_SWQE_FREE:
		/* list からは外されている */
		pib_util_free_send_wqe(qp, send_wqe);
		break;

	case PIB_SWQE_SENDING:
		/* no change */
		break;

	case PIB_SWQE_WAITING:
		list_del_init(&send_wqe->list);
		list_add_tail(&send_wqe->list, &qp->requester.waiting_swqe_head);
		qp->requester.nr_sending_swqe--;
		qp->requester.nr_waiting_swqe++;
		break;

	default:
		pr_emerg("pib: Error qp_type=%s in %s at %s:%u\n",
			 pib_get_qp_type(qp->qp_type), __FUNCTION__, __FILE__, __LINE__);
		BUG();
	}

done:
	pib_util_reschedule_qp(qp); /* 必要の応じてスケジューラから抜くために呼び出す */

	spin_unlock_irqrestore(&qp->lock, flags);

	if (dev->thread.ready_to_send)
		process_sendmsg(dev);

	if (dev->thread.flags & PIB_THREAD_READY_TO_RECV)
		return;

	spin_lock_irqsave(&dev->qp_sched.lock, flags);
	if (time_after(dev->qp_sched.wakeup_time, jiffies)) {
		spin_unlock_irqrestore(&dev->qp_sched.lock, flags);
		return;
	}
	spin_unlock_irqrestore(&dev->qp_sched.lock, flags);

	cond_resched();

	goto restart;
}


static int process_new_send_wr(struct pib_qp *qp)
{
	struct pib_send_wqe *send_wqe;
	u32 num_packets;
	unsigned long now;

	if (qp->state != IB_QPS_RTS)
		return 0;

	if (list_empty(&qp->requester.submitted_swqe_head))
		return 0;

	send_wqe = list_first_entry(&qp->requester.submitted_swqe_head, struct pib_send_wqe, list);

	/*
	 *  A work request with the fence attribute set shall block
	 *  until all prior RDMA READ and Atomic WRs have completed.
	 *  
	 */
	if (send_wqe->send_flags & IB_SEND_FENCE)
		if (0 < qp->requester.nr_rd_atomic)
			return 0;

	if (pib_is_wr_opcode_rd_atomic(send_wqe->opcode)) {
		if (qp->ib_qp_attr.max_rd_atomic <= qp->requester.nr_rd_atomic)
			return 0;
		qp->requester.nr_rd_atomic++;
	}

	list_del_init(&send_wqe->list);
	list_add_tail(&send_wqe->list, &qp->requester.sending_swqe_head);
	qp->requester.nr_submitted_swqe--;
	qp->requester.nr_sending_swqe++;

	send_wqe->processing.list_type = PIB_SWQE_SENDING;

	/*
	 *  Set expected PSN for SQ and etc.
	 */
	now = jiffies;

	num_packets = pib_get_num_of_packets(qp, send_wqe->total_length);

	send_wqe->processing.based_psn     = qp->requester.expected_psn;
	send_wqe->processing.expected_psn  = qp->requester.expected_psn + num_packets;

	send_wqe->processing.all_packets   = num_packets;
	send_wqe->processing.ack_packets   = 0;
	send_wqe->processing.sent_packets  = 0;

	qp->requester.expected_psn        += num_packets;

	send_wqe->processing.schedule_time = now;
	send_wqe->processing.local_ack_time = now + PIB_SCHED_TIMEOUT;

	send_wqe->processing.retry_cnt     = qp->ib_qp_attr.retry_cnt;
	send_wqe->processing.rnr_retry     = qp->ib_qp_attr.rnr_retry;

	return 1;
}


/*
 *  state は RTS
 *
 *  Lock: qp
 */
static int process_send_wr(struct pib_dev *dev, struct pib_qp *qp, struct pib_send_wqe *send_wqe)
{
	enum ib_wr_opcode opcode;
	enum ib_wc_status status;

	BUG_ON(send_wqe->processing.list_type != PIB_SWQE_SENDING);

	status = send_wqe->processing.status;
	opcode = send_wqe->opcode;

	/* 処理中にエラーになったが前方の SEND WR が処理完了するまで遅延していた */
	if (status != IB_WC_SUCCESS)
		goto completion_error;

	switch (qp->qp_type) {

	case IB_QPT_RC:
		return pib_process_rc_qp_request(dev, qp, send_wqe);

	case IB_QPT_UD:
	case IB_QPT_GSI:
	case IB_QPT_SMI:
		return pib_process_ud_qp_request(dev, qp, send_wqe);

	default:
		pr_emerg("pib: Error qp_type=%s in %s at %s:%u\n",
			 pib_get_qp_type(qp->qp_type), __FUNCTION__, __FILE__, __LINE__);
		BUG();
	}

	return -1;

completion_error:
	pib_util_insert_wc_error(qp->send_cq, qp, send_wqe->wr_id,
				 status, send_wqe->opcode);

	list_del_init(&send_wqe->list);
	qp->requester.nr_sending_swqe--;
	send_wqe->processing.list_type = PIB_SWQE_FREE;

	switch (qp->qp_type) {
	case IB_QPT_RC:
		qp->state = IB_QPS_ERR;
		pib_util_flush_qp(qp, 0);
		break;

	case IB_QPT_UD:
	case IB_QPT_GSI:
	case IB_QPT_SMI:
		qp->state = IB_QPS_SQE;
		pib_util_flush_qp(qp, 1);
		break;

	default:
		pr_emerg("pib: Error qp_type=%s in %s at %s:%u\n",
			 pib_get_qp_type(qp->qp_type), __FUNCTION__, __FILE__, __LINE__);
		BUG();
	}

	return -1;
}


static int process_incoming_message(struct pib_dev *dev, int port_index)
{
	int ret, header_size;
	struct msghdr msghdr = {.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL};
	struct kvec iov;
	struct pib_port *port;
	void *buffer;
	struct pib_packet_lrh *lrh;
	struct ib_grh         *grh;
	struct pib_packet_bth *bth;
	u32 dest_qp_num;
	u16 dlid;

	buffer = dev->thread.buffer;

	iov.iov_base = buffer;
	iov.iov_len  = PIB_PACKET_BUFFER;

	port = &dev->ports[port_index];

	ret = kernel_recvmsg(port->socket, &msghdr,
			     &iov, 1, iov.iov_len, msghdr.msg_flags);

	if (ret < 0) {
		if (ret == -EINTR)
			set_bit(PIB_THREAD_READY_TO_RECV, &dev->thread.flags);
		return ret;
	} else if (ret == 0)
		return -EAGAIN;

	port->perf.rcv_packets++;
	port->perf.rcv_data += ret;

	header_size = pib_parse_packet_header(buffer, ret, &lrh, &grh, &bth);
	if (header_size < 0) {
		pib_debug("pib: wrong drop packet(size=%u)\n", ret);
		goto silently_drop;
	}

	buffer += header_size;
	ret    -= header_size;

	/* Payload */
	ret -= pib_packet_bth_get_padcnt(bth); /* Pad Count */
	if (ret < 0) {
		pib_debug("pib: drop packet: too small packet except LRH & BTH (size=%u)\n", ret);
		goto silently_drop;
	}

	dlid        = be16_to_cpu(lrh->dlid);
	dest_qp_num = be32_to_cpu(bth->destQP) & PIB_QPN_MASK;

	if ((dest_qp_num == PIB_QP0) || (dlid < PIB_MCAST_LID_BASE)) {
		/* Unicast */
		process_incoming_message_per_qp(dev, port_index, dlid, dest_qp_num,
						lrh, grh, bth, buffer, ret);
	} else {
		/* Multicast */
		int i, max;
		struct pib_packet_deth *deth;
		u16 port_lid, slid;
		u32 src_qp_num;
		struct pib_mcast_link *mcast_link;
		u32 qp_nums[PIB_MCAST_QP_ATTACH];
		unsigned long flags;

		if ((bth->OpCode != IB_OPCODE_UD_SEND_ONLY) && 
		    (bth->OpCode != IB_OPCODE_UD_SEND_ONLY_WITH_IMMEDIATE)) {
			pib_debug("pib: drop packet: \n");
			goto silently_drop;
		}

		if (ret < sizeof(struct pib_packet_deth))
			goto silently_drop;

		deth = (struct pib_packet_deth*)buffer;

		src_qp_num = be32_to_cpu(deth->srcQP) & PIB_QPN_MASK;

		spin_lock_irqsave(&dev->lock, flags);
		i=0;
		list_for_each_entry(mcast_link, &dev->mcast_table[dlid - PIB_MCAST_LID_BASE], lid_list) {
			qp_nums[i] = mcast_link->qp_num;
			i++;
		}
		spin_unlock_irqrestore(&dev->lock, flags);

		max = i;

		port_lid = port->ib_port_attr.lid;
		slid     = be16_to_cpu(lrh->slid);

		/* 
		 * マルチキャストパケットを届ける QP は複数かもしれない。
		 * ただし送信した QP 自身は受け取らない。
		 */
		for (i=0 ; i<max ; i++) {
			if ((port_lid == slid) && (src_qp_num == qp_nums[i]))
				continue;

			pib_debug("pib: MC packet qp_num=0x%06x\n", qp_nums[i]);
			process_incoming_message_per_qp(dev, port_index, dlid, qp_nums[i],
							lrh, grh, bth, buffer, ret);

			cond_resched();
		}
	}

silently_drop:
	return 0;
}


int pib_parse_packet_header(void *buffer, int size, struct pib_packet_lrh **lrh_p, struct ib_grh **grh_p, struct pib_packet_bth **bth_p)
{
	int ret = 0;
	struct pib_packet_lrh *lrh;
	struct ib_grh         *grh = NULL;
	struct pib_packet_bth *bth;
	u8 lnh;

	/* Analyze Local Route Hedaer */
	lrh = (struct pib_packet_lrh *)buffer;

	if (size < sizeof(*lrh))
		return -1;

	/* check packet length */
	if (pib_packet_lrh_get_pktlen(lrh) * 4 != size)
		return -1;

	buffer += sizeof(*lrh);
	size   -= sizeof(*lrh);
	ret    += sizeof(*lrh); 

	/* check Link Version */
	if ((lrh->vl_lver & 0xF) != 0)
		return -2;

	/* check Transport &  Next Header */
	lnh = (lrh->sl_rsv_lnh & 0x3);

	if (lnh == 0x2)
		/* IBA local */
		goto skip_grh;
	else if (lnh != 0x3)
		return -3;

	/* IBA global */
	grh = (struct ib_grh *)buffer;

	if (size < sizeof(*grh))
		return -1;

	buffer += sizeof(*grh);
	size   -= sizeof(*grh);
	ret    += sizeof(*grh); 

skip_grh:
	/* Base Transport Header */
	bth = (struct pib_packet_bth *)buffer;
	    
	if (size < sizeof(*bth))
		return -1;

	ret    += sizeof(*bth);

	*lrh_p = lrh;
	*grh_p = grh;
	*bth_p = bth;

	return ret;
}


static void process_incoming_message_per_qp(struct pib_dev *dev, int port_index, u16 dlid, u32 dest_qp_num, struct pib_packet_lrh *lrh, struct ib_grh *grh, struct pib_packet_bth *bth, void *buffer, int size)
{
	unsigned long flags;
	struct pib_qp *qp;
	const u8 port_num = port_index + 1;

	spin_lock_irqsave(&dev->lock, flags);

	switch (dest_qp_num) {

	case PIB_QP0:
	case PIB_QP1:
		qp = dev->ports[port_index].qp_info[dest_qp_num];
		break;

	case IB_MULTICAST_QPN:
		BUG();
		break;

	default:
		qp = pib_util_find_qp(dev, dest_qp_num);
		break;
	}

	if (qp == NULL) {
		spin_unlock_irqrestore(&dev->lock, flags);
		pib_debug("pib: drop packet: not found qp (qpn=0x%06x)\n", dest_qp_num);
		goto silently_drop;
	}

	/* LRH: check port LID and DLID of incoming packet */
	if (((dest_qp_num == PIB_QP0) && (dlid == PIB_LID_PERMISSIVE)))
		;
	else if (!pib_is_unicast_lid(dlid))
		;
	else if (dlid != dev->ports[port_index].ib_port_attr.lid) {
		spin_unlock_irqrestore(&dev->lock, flags);
		pib_debug("pib: drop packet: differ packet's dlid from port lid (0x%04x, 0x%04x)\n",
			  dlid, dev->ports[port_index].ib_port_attr.lid);
		goto silently_drop;
	}

	/* @notice ロックの入れ子関係を一部崩している */
	spin_lock(&qp->lock);
	spin_unlock(&dev->lock);

	switch (qp->qp_type) {

	case IB_QPT_RC:
		pib_receive_rc_qp_incoming_message(dev, port_num, qp, lrh, grh, bth, buffer, size);
		break;

	case IB_QPT_UD:
	case IB_QPT_GSI:
	case IB_QPT_SMI:
		pib_receive_ud_qp_incoming_message(dev, port_num, qp, lrh, grh, bth, buffer, size);
		break;

	default:
		pr_emerg("pib: Error qp_type=%s in %s at %s:%u\n",
			 pib_get_qp_type(qp->qp_type), __FUNCTION__, __FILE__, __LINE__);
		BUG();
	}

	pib_util_reschedule_qp(qp);	

	spin_unlock_irqrestore(&qp->lock, flags);

	if (dev->thread.ready_to_send)
		process_sendmsg(dev);

silently_drop:

	return;
}



/******************************************************************************/
/*                                                                            */
/******************************************************************************/

void pib_util_reschedule_qp(struct pib_qp *qp)
{
	struct pib_dev *dev;
	unsigned long flags;
	unsigned long now, schedule_time;
	struct pib_send_wqe *send_wqe;
	struct rb_node **link;
	struct rb_node *parent = NULL;
	struct rb_node *rb_node;

	dev = to_pdev(qp->ib_qp.device);

	/************************************************************/
	/* Red/Black tree からの取り外し                            */
	/************************************************************/

	spin_lock_irqsave(&dev->qp_sched.lock, flags);
	if (qp->sched.on) {
		qp->sched.on = 0;
		rb_erase(&qp->sched.rb_node, &dev->qp_sched.rb_root);
	}
	spin_unlock_irqrestore(&dev->qp_sched.lock, flags);

	/************************************************************/
	/* 再計算                                                   */
	/************************************************************/
	now = jiffies;
	schedule_time = now + PIB_SCHED_TIMEOUT;

	if ((qp->qp_type == IB_QPT_RC) && pib_is_recv_ok(qp->state))
		if (!list_empty(&qp->responder.ack_head)) {
			schedule_time = now;
			goto skip;
		}

	if ((qp->state != IB_QPS_RTS) && (qp->state != IB_QPS_SQD))
		return;

	if (!list_empty(&qp->requester.waiting_swqe_head)) {
		send_wqe = list_first_entry(&qp->requester.waiting_swqe_head, struct pib_send_wqe, list);

		if (time_before(send_wqe->processing.local_ack_time, schedule_time))
			schedule_time = send_wqe->processing.local_ack_time;
	}

	if (!list_empty(&qp->requester.sending_swqe_head)) {
		send_wqe = list_first_entry(&qp->requester.sending_swqe_head, struct pib_send_wqe, list);

		if (send_wqe->processing.status != IB_WC_SUCCESS)
			if (!list_empty(&qp->requester.waiting_swqe_head))
				goto skip;

		if (time_before(send_wqe->processing.schedule_time, schedule_time))
			schedule_time = send_wqe->processing.schedule_time;
	}

	if ((qp->state == IB_QPS_RTS) && !list_empty(&qp->requester.submitted_swqe_head)) {
		send_wqe = list_first_entry(&qp->requester.submitted_swqe_head, struct pib_send_wqe, list);

		if (pib_is_wr_opcode_rd_atomic(send_wqe->opcode))
			if (qp->ib_qp_attr.max_rd_atomic <= qp->requester.nr_rd_atomic)
				goto skip;

		schedule_time = now;
	}

skip:
	if (schedule_time == now + PIB_SCHED_TIMEOUT)
		return;

	qp->sched.time = schedule_time;
	qp->sched.tid  = dev->qp_sched.master_tid;

	/************************************************************/
	/* Red/Black tree への登録                                  */
	/************************************************************/
	spin_lock_irqsave(&dev->qp_sched.lock, flags);
	link = &dev->qp_sched.rb_root.rb_node;
	while (*link) {
		int cond;
		struct pib_qp *qp_tmp;

		parent = *link;
		qp_tmp = rb_entry(parent, struct pib_qp, sched.rb_node);

		if (qp_tmp->sched.time != schedule_time)
			cond = time_after(qp_tmp->sched.time, schedule_time);
		else
			cond = ((long)(qp_tmp->sched.tid - qp->sched.tid) > 0);

		if (cond)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	rb_link_node(&qp->sched.rb_node, parent, link);
	rb_insert_color(&qp->sched.rb_node, &dev->qp_sched.rb_root);
	qp->sched.on = 1;

	/* calculate the most early time  */
	rb_node = rb_first(&dev->qp_sched.rb_root);
	BUG_ON(rb_node == NULL);
	qp = rb_entry(rb_node, struct pib_qp, sched.rb_node);
	dev->qp_sched.wakeup_time = qp->sched.time;

	spin_unlock_irqrestore(&dev->qp_sched.lock, flags);

	if (time_before_eq(dev->qp_sched.wakeup_time, now))
		set_bit(PIB_THREAD_QP_SCHEDULE, &dev->thread.flags);
}


struct pib_qp *pib_util_get_first_scheduling_qp(struct pib_dev *dev)
{
	unsigned long flags;
	struct rb_node *rb_node;
	struct pib_qp *qp = NULL;

	spin_lock_irqsave(&dev->qp_sched.lock, flags);

	rb_node = rb_first(&dev->qp_sched.rb_root);

	if (rb_node == NULL)
		goto done;

	qp = rb_entry(rb_node, struct pib_qp, sched.rb_node);
done:

	spin_unlock_irqrestore(&dev->qp_sched.lock, flags);

	return qp;
}


/******************************************************************************/
/*                                                                            */
/******************************************************************************/

static void process_on_wq_scheduler(struct pib_dev *dev)
{
	unsigned long flags;
	struct pib_work_struct *work;

retry:
	spin_lock_irqsave(&dev->lock, flags);
	spin_lock(&dev->wq_sched.lock);

	if (list_empty(&dev->wq_sched.head)) {
		spin_unlock(&dev->wq_sched.lock);
		spin_unlock_irqrestore(&dev->lock, flags);
		return;
	}
	spin_unlock(&dev->wq_sched.lock);
	
	work = list_first_entry(&dev->wq_sched.head, struct pib_work_struct, entry);
	list_del_init(&work->entry);

	work->func(work);

	spin_unlock_irqrestore(&dev->lock, flags);

	goto retry;
}


void pib_queue_work(struct pib_dev *dev, struct pib_work_struct *work)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->wq_sched.lock, flags);
	list_add_tail(&work->entry, &dev->wq_sched.head);
	spin_unlock_irqrestore(&dev->wq_sched.lock, flags);

	set_bit(PIB_THREAD_WQ_SCHEDULE, &dev->thread.flags);
	complete(&dev->thread.completion);
}


void pib_cancel_work(struct pib_dev *dev, struct pib_work_struct *work)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->wq_sched.lock, flags);
	list_del_init(&work->entry);
	spin_unlock_irqrestore(&dev->wq_sched.lock, flags);
}


/******************************************************************************/
/*                                                                            */
/******************************************************************************/
static void process_sendmsg(struct pib_dev *dev)
{
	int ret;
	u8 port_num;
	u32 src_qp_num;
	u16 slid;
	u16 dlid;
	struct sockaddr *sockaddr;
	struct msghdr	msghdr;
	struct kvec	iov;
	struct pib_port *port;

	port_num   = dev->thread.port_num;
	src_qp_num = dev->thread.src_qp_num;
	slid       = dev->thread.slid;
	dlid       = dev->thread.dlid;

	sockaddr = get_sockaddr_from_dlid(dev, port_num, src_qp_num, dlid);
	if (!sockaddr) {
		pr_err("pib: Not found the destination address in ld_table (dlid=%u)", dlid);
		goto done;
	}

	BUG_ON(dev->thread.msg_size == 0);

	/* QP0 以外は SLID または DLID が 0 のパケットは投げない */
	if (src_qp_num != PIB_QP0)
		if ((slid == 0) || (dlid == 0))
			goto done;

	memset(&msghdr, 0, sizeof(msghdr));

	msghdr.msg_name    = sockaddr;
	msghdr.msg_namelen = (sockaddr->sa_family == AF_INET6) ?
		sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);

	iov.iov_base = dev->thread.buffer;
	iov.iov_len  = dev->thread.msg_size;

	port = &dev->ports[port_num - 1];

	ret = kernel_sendmsg(port->socket, &msghdr, &iov, 1, iov.iov_len);

	if (ret > 0) {
		port->perf.xmit_packets++;
		port->perf.xmit_data += ret;
	}

	if (pib_is_unicast_lid(dlid))
		goto done;

	/*
	 * マルチキャストの場合、同じ HCA に同一の multicast group の受け取りを
	 * する別の QP がある可能性があるので、loopback にも送信する。
	 */
	sockaddr = port->sockaddr;

	msghdr.msg_name    = sockaddr;
	msghdr.msg_namelen = (sockaddr->sa_family == AF_INET6) ?
		sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);

	ret = kernel_sendmsg(port->socket, &msghdr, &iov, 1, iov.iov_len);

done:
	dev->thread.ready_to_send = 0;
}


static struct sockaddr *
get_sockaddr_from_dlid(struct pib_dev *dev, u8 port_num, u32 src_qp_num, u16 dlid)
{
	unsigned long flags;
	struct sockaddr *sockaddr = NULL;

	if (src_qp_num != PIB_QP0) {
		if (dlid == 0)
			sockaddr = dev->ports[port_num - 1].sockaddr;
		else if (dlid == dev->ports[port_num - 1].ib_port_attr.lid)
			/* loopback */
			sockaddr = dev->ports[port_num - 1].sockaddr;
		else if (dlid < PIB_MCAST_LID_BASE)
			/* unicast */
			sockaddr = dev->ports[port_num - 1].lid_table[dlid];
	}

	if (sockaddr)
		return sockaddr;

	/* multicast packets or packets to switch */
	spin_lock_irqsave(&pib_easy_sw.lock, flags);
	sockaddr = pib_easy_sw.sockaddr;
	spin_unlock_irqrestore(&pib_easy_sw.lock, flags);

	return sockaddr;
}


/******************************************************************************/
/*                                                                            */
/******************************************************************************/

static void sock_data_ready_callback(struct sock *sk, int bytes)
{
	struct pib_dev* dev  = (struct pib_dev*)sk->sk_user_data;

	set_bit(PIB_THREAD_READY_TO_RECV, &dev->thread.flags);
	complete(&dev->thread.completion);
}


static void timer_timeout_callback(unsigned long opaque)
{
	struct pib_dev* dev  = (struct pib_dev*)opaque;
	
	set_bit(PIB_THREAD_QP_SCHEDULE, &dev->thread.flags);
	complete(&dev->thread.completion);
}
