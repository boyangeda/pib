/*
 * pib_cq.c - Completion Queue(CQ) functions
 *
 * Copyright (c) 2013-2015 Minoru NAKAMURA <nminoru@nminoru.jp>
 *
 * This code is licenced under the GPL version 2 or BSD license.
 */
#include <linux/module.h>
#include <linux/init.h>

#include "pib.h"
#include "pib_spinlock.h"
#include "pib_trace.h"


static int insert_wc(struct pib_cq *cq, const struct ib_wc *wc, int solicited);
static void cq_overflow_handler(struct pib_work_struct *work);


static struct ib_cq *
create_cq(struct ib_device *ibdev, int entries, int vector, int dummy,
	  struct ib_ucontext *context,
	  struct ib_udata *udata)
{
	int i;
	struct pib_dev *dev;
	struct pib_cq *cq;
	struct pib_cqe *cqe, *cqe_next;
	unsigned long flags;
	u32 cq_num;

	if (!ibdev)
		return ERR_PTR(-EINVAL);

	dev = to_pdev(ibdev);

	if (entries < 1 || dev->ib_dev_attr.max_cqe <= entries)
		return ERR_PTR(-EINVAL);

	if (dev->ib_dev_attr.max_cq <= dev->nr_cq)
		return ERR_PTR(-ENOMEM);

	cq = kmem_cache_zalloc(pib_cq_cachep, GFP_KERNEL);
	if (!cq)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&cq->list);
	getnstimeofday(&cq->creation_time);

	spin_lock_irqsave(&dev->lock, flags);
	cq_num = pib_alloc_obj_num(dev, PIB_BITMAP_CQ_START, PIB_MAX_CQ, &dev->last_cq_num);
	if (cq_num == (u32)-1) {
		spin_unlock_irqrestore(&dev->lock, flags);
		goto err_alloc_cq_num;
	}
	dev->nr_cq++;
	list_add_tail(&cq->list, &dev->cq_head);
	cq->cq_num = cq_num;
	spin_unlock_irqrestore(&dev->lock, flags);

	cq->state	= PIB_STATE_OK;
	cq->notify_flag = 0;
	cq->has_notified= 1; /* assume CQ has been notified when initial */

	cq->ib_cq.cqe	= entries;
	cq->nr_cqe	= 0;

	pib_spin_lock_init(&cq->lock);

	INIT_LIST_HEAD(&cq->cqe_head);
	INIT_LIST_HEAD(&cq->free_cqe_head);
	PIB_INIT_WORK(&cq->work, dev, cq, cq_overflow_handler);

	/* allocate CQE internally */

	for (i=0 ; i<entries ; i++) {
		struct pib_cqe *cqe;

		cqe = kmem_cache_zalloc(pib_cqe_cachep, GFP_KERNEL);
		if (!cqe)
			goto err_allloc_ceq;
		
		INIT_LIST_HEAD(&cqe->list);
		list_add_tail(&cqe->list, &cq->free_cqe_head);
	}

	pib_trace_api(dev, IB_USER_VERBS_CMD_CREATE_CQ, cq_num);

	return &cq->ib_cq;

err_allloc_ceq:
	list_for_each_entry_safe(cqe, cqe_next, &cq->free_cqe_head, list) {
		list_del_init(&cqe->list);
		kmem_cache_free(pib_cqe_cachep, cqe);
	}

	spin_lock_irqsave(&dev->lock, flags);
	list_del(&cq->list);
	dev->nr_cq--;
	pib_dealloc_obj_num(dev, PIB_BITMAP_CQ_START, cq_num);
	spin_unlock_irqrestore(&dev->lock, flags);

err_alloc_cq_num:
	kmem_cache_free(pib_cq_cachep, cq);

	return ERR_PTR(-ENOMEM);
}


#ifdef PIB_CQ_FLAGS_TIMESTAMP_COMPLETION_SUPPORT
struct ib_cq *pib_create_cq(struct ib_device *ibdev,
			    const struct ib_cq_init_attr *attr,
			    struct ib_ucontext *context,
			    struct ib_udata *udata)
{
	return create_cq(ibdev, attr->cqe, attr->comp_vector, attr->flags, context, udata);
}
#else
struct ib_cq *pib_create_cq(struct ib_device *ibdev, int entries, int vector,
			    struct ib_ucontext *context,
			    struct ib_udata *udata)
{
	return create_cq(ibdev, entries, vector, 0, context, udata);
}
#endif


int pib_destroy_cq(struct ib_cq *ibcq)
{
	struct pib_dev *dev;
	struct pib_cq *cq;
	struct pib_cqe *cqe, *cqe_next;
	unsigned long flags;

	if (!ibcq)
		return 0;

	dev = to_pdev(ibcq->device);
	cq = to_pcq(ibcq);

	pib_trace_api(dev, IB_USER_VERBS_CMD_DESTROY_CQ, cq->cq_num);

	pib_spin_lock_irqsave(&cq->lock, flags);
	list_for_each_entry_safe(cqe, cqe_next, &cq->cqe_head, list) {
		list_del_init(&cqe->list);
		kmem_cache_free(pib_cqe_cachep, cqe);
	}
	list_for_each_entry_safe(cqe, cqe_next, &cq->free_cqe_head, list) {
		list_del_init(&cqe->list);
		kmem_cache_free(pib_cqe_cachep, cqe);
	}
	cq->nr_cqe = 0;
	pib_spin_unlock_irqrestore(&cq->lock, flags);

	spin_lock_irqsave(&dev->lock, flags);
	list_del(&cq->list);
	dev->nr_cq--;
	pib_dealloc_obj_num(dev, PIB_BITMAP_CQ_START, cq->cq_num);

	pib_cancel_work(dev, &cq->work);
	spin_unlock_irqrestore(&dev->lock, flags);

	kmem_cache_free(pib_cq_cachep, cq);

	return 0;
}


int pib_modify_cq(struct ib_cq *ibcq, u16 cq_count, u16 cq_period)
{
	struct pib_dev *dev;
	struct pib_cq *cq;

	pr_err("pib: pib_modify_cq\n");

	if (!ibcq)
		return -EINVAL;

	dev = to_pdev(ibcq->device);
	cq = to_pcq(ibcq);

	pib_trace_api(dev, PIB_USER_VERBS_CMD_MODIFY_CQ, cq->cq_num);

	return 0;
}


int pib_resize_cq(struct ib_cq *ibcq, int entries, struct ib_udata *udata)
{
	struct pib_dev *dev;
	struct pib_cq *cq;

	pr_err("pib: pib_resize_cq\n");

	if (!ibcq)
		return -EINVAL;

	dev = to_pdev(ibcq->device);
	cq = to_pcq(ibcq);

	pib_trace_api(dev, IB_USER_VERBS_CMD_RESIZE_CQ, cq->cq_num);

	return 0;
}


int pib_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *ibwc)
{
	int i, ret = 0;
	struct pib_dev *dev;
	struct pib_cq *cq;
	unsigned long flags;

	if (!ibcq)
		return -EINVAL;

	dev = to_pdev(ibcq->device);
	cq = to_pcq(ibcq);

	pib_trace_api(dev, IB_USER_VERBS_CMD_POLL_CQ, cq->cq_num);

	pib_spin_lock_irqsave(&cq->lock, flags);

	if (cq->state != PIB_STATE_OK) {
		ret = -EACCES;
		goto done;
	}

	for (i=0 ; (i<num_entries) && !list_empty(&cq->cqe_head) ; i++) {
		struct pib_cqe *cqe;

		cqe = list_first_entry(&cq->cqe_head, struct pib_cqe, list);
		list_del_init(&cqe->list);
		list_add_tail(&cqe->list, &cq->free_cqe_head);

		ibwc[i] = cqe->ib_wc;

		cq->nr_cqe--;
		ret++;
	}

done:
	pib_spin_unlock_irqrestore(&cq->lock, flags);

	return ret;
}


int pib_req_notify_cq(struct ib_cq *ibcq, enum ib_cq_notify_flags notify_flags)
{
	struct pib_dev *dev;
	struct pib_cq *cq;
	unsigned long flags;
	int ret = 0;

	if (!ibcq)
		return -EINVAL;

	dev = to_pdev(ibcq->device);
	cq = to_pcq(ibcq);

	pib_trace_api(dev, IB_USER_VERBS_CMD_REQ_NOTIFY_CQ, cq->cq_num);

	pib_spin_lock_irqsave(&cq->lock, flags);

	if (cq->state != PIB_STATE_OK)
		ret = -1;
	else {
		if (notify_flags & IB_CQ_SOLICITED)
			cq->notify_flag = IB_CQ_SOLICITED;
		else if (notify_flags & IB_CQ_NEXT_COMP)
			cq->notify_flag = IB_CQ_NEXT_COMP;
		
		if ((notify_flags & IB_CQ_REPORT_MISSED_EVENTS) &&
			 !list_empty(&cq->cqe_head))
			ret = 1;

		/* @note CQE ??????????????????????????? req_notify_cq ??????????????????????????????????????????????????? */
		cq->has_notified = 0;
	}

	pib_spin_unlock_irqrestore(&cq->lock, flags);

	return ret;
}


/**
 *  QP ??? RESET ???????????????????????????????????? CQ ????????????
 *  @return ???????????? WC ???
 */
int pib_util_remove_cq(struct pib_cq *cq, struct pib_qp *qp)
{
	int count = 0;
	unsigned long flags;
	struct pib_cqe *cqe, *cqe_next;

	BUG_ON(qp == NULL);

	pib_spin_lock_irqsave(&cq->lock, flags);
	list_for_each_entry_safe(cqe, cqe_next, &cq->cqe_head, list) {
		if (cqe->ib_wc.qp == &qp->ib_qp) {
			cq->nr_cqe--;
			list_del_init(&cqe->list);
			list_add_tail(&cqe->list, &cq->free_cqe_head);
			count++;
		}
	}
	pib_spin_unlock_irqrestore(&cq->lock, flags);

	return count;
}


int pib_util_insert_wc_success(struct pib_cq *cq, const struct ib_wc *wc, int solicited)
{
	return insert_wc(cq, wc, solicited);
}


int pib_util_insert_wc_error(struct pib_cq *cq, struct pib_qp *qp, u64 wr_id, enum ib_wc_status status, enum ib_wc_opcode opcode)
{
	struct ib_wc wc = {
		.wr_id    = wr_id,
		.status   = status,
		.opcode   = opcode,
		.qp       = &qp->ib_qp,
	};

	if (pib_get_behavior(PIB_BEHAVIOR_CORRUPT_INVALID_WC_ATTRS)) {
		wc.opcode         = pib_random();
		wc.byte_len       = pib_random();
		wc.ex.imm_data    = pib_random();
		wc.wc_flags       = pib_random();
		wc.pkey_index     = pib_random();
		wc.slid           = pib_random();
		wc.sl             = pib_random();
		wc.dlid_path_bits = pib_random();
	}

	return insert_wc(cq, &wc, 1);
}


static int insert_wc(struct pib_cq *cq, const struct ib_wc *wc, int solicited)
{
	int ret;
	unsigned long flags;
	struct pib_cqe *cqe;

	pib_trace_comp(to_pdev(cq->ib_cq.device), cq, wc);

	pib_spin_lock_irqsave(&cq->lock, flags);

	if (cq->state != PIB_STATE_OK) {
		ret = -EACCES;
		goto done;
	}

	if (list_empty(&cq->free_cqe_head)) {
		/* CQ overflow */
		cq->state     = PIB_STATE_ERR;
		pib_queue_work(to_pdev(cq->ib_cq.device), &cq->work);

		ret = -ENOMEM;
		goto done;
	}

	cqe = list_first_entry(&cq->free_cqe_head, struct pib_cqe, list);
	list_del_init(&cqe->list);

	cqe->ib_wc = *wc;

	if (to_pqp(wc->qp)->qp_type == IB_QPT_SMI)
		cqe->ib_wc.port_num = to_pqp(wc->qp)->ib_qp_init_attr.port_num;

	cq->nr_cqe++;

	list_add_tail(&cqe->list, &cq->cqe_head);

	/* tell completion channel */
	if ((cq->notify_flag == IB_CQ_NEXT_COMP) ||
	    ((cq->notify_flag == IB_CQ_SOLICITED) && solicited)) {
		if (!cq->has_notified) {
			/*
			 * The cq->has_notified must be set to 1 before calling completion handler. 
			 * Because pib_req_notify_cq() may be called via cq completion handler.
			 */
			cq->has_notified = 1;
			cq->ib_cq.comp_handler(&cq->ib_cq, cq->ib_cq.cq_context);
		}
	}

	ret = 0;

done:
	pib_spin_unlock_irqrestore(&cq->lock, flags);

	return ret;
}


void pib_util_insert_async_cq_error(struct pib_dev *dev, struct pib_cq *cq)
{
	struct ib_event ev;
	struct pib_qp *qp;
	unsigned long flags;

	pib_trace_async(dev, IB_EVENT_CQ_ERR, cq->cq_num);

	pib_spin_lock_irqsave(&cq->lock, flags);
	cq->state     = PIB_STATE_ERR;
	ev.event      = IB_EVENT_CQ_ERR;
	ev.device     = cq->ib_cq.device;
	ev.element.cq = &cq->ib_cq;
	cq->ib_cq.event_handler(&ev, cq->ib_cq.cq_context);
	pib_spin_unlock_irqrestore(&cq->lock, flags);

	/* ???????????? cq ????????????????????? */

	list_for_each_entry(qp, &dev->qp_head, list) {
		pib_spin_lock(&qp->lock);
		if ((cq == qp->send_cq) || (cq == qp->recv_cq)) {
			qp->state = IB_QPS_ERR;
			pib_util_flush_qp(qp, 0);
			pib_util_insert_async_qp_error(qp, IB_EVENT_QP_FATAL);
		}
		pib_spin_unlock(&qp->lock);
	}
}


static void cq_overflow_handler(struct pib_work_struct *work)
{
	struct pib_cq *cq = work->data;
	struct pib_dev *dev = work->dev;

	BUG_ON(!spin_is_locked(&dev->lock));

	pib_util_insert_async_cq_error(dev, cq);
}
