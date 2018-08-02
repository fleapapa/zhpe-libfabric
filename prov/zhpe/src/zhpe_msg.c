/*
 * Copyright (c) 2014-2015 Intel Corporation, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Hewlett Packard Enterprise Development LP.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You m"ay choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <zhpe.h>

#define ZHPE_LOG_DBG(...) _ZHPE_LOG_DBG(FI_LOG_EP_DATA, __VA_ARGS__)
#define ZHPE_LOG_ERROR(...) _ZHPE_LOG_ERROR(FI_LOG_EP_DATA, __VA_ARGS__)

static inline ssize_t do_recvmsg(struct fid_ep *ep, const void *vmsg,
				 uint64_t flags, bool tagged)
{
	ssize_t			ret = -FI_EINVAL;
	struct zhpe_rx_entry	*rx_entry = NULL;
	size_t			i;
	size_t			j;
	struct zhpe_ep		*zhpe_ep;
	uint64_t		op_flags;
	const struct fi_msg	*msg;
	const struct fi_msg_tagged *tmsg;
	size_t			iov_count;
	const struct iovec	*iov;
	void			**desc;
	fi_addr_t		fiaddr;
	void			*context;
	uint64_t		tag;
	uint64_t		ignore;
	struct zhpe_rx_ctx	*rx_ctx;
	struct zhpe_mr		*zmr;
	struct zhpe_rx_entry	*rx_claimed;

	switch (ep->fid.fclass) {
	case FI_CLASS_EP:
		zhpe_ep = container_of(ep, struct zhpe_ep, ep);
		rx_ctx = zhpe_ep->attr->rx_ctx;
		op_flags = zhpe_ep->rx_attr.op_flags;
		break;
	case FI_CLASS_RX_CTX:
	case FI_CLASS_SRX_CTX:
		rx_ctx = container_of(ep, struct zhpe_rx_ctx, ctx);
		op_flags = rx_ctx->attr.op_flags;
		break;
	default:
		ZHPE_LOG_ERROR("Invalid ep type\n");
		ret = -FI_EINVAL;
		goto done;
	}

	if (!rx_ctx->enabled) {
		ret = -FI_EOPBADSTATE;
		goto done;
	}

	if (likely(!(flags & ZHPE_TRIGGERED_OP))) {
		if (flags &
		    ~(ZHPE_NO_COMPLETION | ZHPE_USE_OP_FLAGS |
		      FI_COMPLETION | FI_TRIGGER | FI_MULTI_RECV |
		      FI_PEEK | FI_CLAIM | FI_DISCARD | FI_MSG | FI_RECV))
			goto done;
		if ((flags & (FI_PEEK | FI_CLAIM | FI_DISCARD)) == FI_DISCARD)
			goto done;
		flags |= (FI_MSG | FI_RECV);

		if (flags & ZHPE_USE_OP_FLAGS)
			flags |= op_flags;
		else if (!rx_ctx->comp.recv_cq_event)
			/* recvmsg && no FI_SELECTIVE_COMPLETION */
			flags |= FI_COMPLETION;
	}

	if (flags & FI_TRIGGER) {
		if (tagged)
			ret = zhpe_queue_tmsg_op(ep, vmsg, flags, FI_OP_TRECV);
		else
			ret = zhpe_queue_tmsg_op(ep, vmsg, flags, FI_OP_RECV);
		if (ret != 1)
			goto done;
	}

	if (tagged) {
		flags |= FI_TAGGED;
		tmsg = vmsg;
		iov_count = tmsg->iov_count;
		iov = tmsg->msg_iov;
		desc = tmsg->desc;
		fiaddr = tmsg->addr;
		context = tmsg->context;
		tag = tmsg->tag;
		ignore = tmsg->ignore;
	} else {
		msg = vmsg;
		iov_count = msg->iov_count;
		iov = msg->msg_iov;
		desc = msg->desc;
		fiaddr = msg->addr;
		context = msg->context;
		tag = 0;
		ignore = ~tag;
	}

	if (flags & FI_DISCARD)
		iov_count = 0;
	if (iov_count > ZHPE_EP_MAX_IOV_LIMIT)
		goto done;
	if (iov_count > 1 && (flags & FI_MULTI_RECV))
		goto done;

	fiaddr = ((rx_ctx->attr.caps & FI_DIRECTED_RECV) ?
		  fiaddr : FI_ADDR_UNSPEC);

	if (flags & FI_PEEK) {
		zhpe_pe_rx_peek_recv(rx_ctx, fiaddr, tag, ignore, flags,
				     context);
		ret = 0;
		goto done;
	}

	fastlock_acquire(&rx_ctx->lock);
	rx_entry = zhpe_rx_new_entry(rx_ctx);
	fastlock_release(&rx_ctx->lock);
	if (!rx_entry) {
		ret = -FI_ENOMEM;
		goto done;
	}
	rx_entry->buffered = ZHPE_RX_BUF_USER;
	rx_entry->rx_state = ZHPE_RX_STATE_IDLE;
	rx_entry->flags = flags;
	rx_entry->addr = fiaddr;
	rx_entry->tag = tag;
	rx_entry->ignore = ignore;
	rx_entry->context = context;
	rx_entry->rstate.cnt = 0;
	rx_entry->lstate.cnt = 0;
	rx_entry->total_len = 0;

	for (i = 0, j = 0; i < iov_count; i++) {
		if (!iov[i].iov_len)
			continue;
		rx_entry->liov[j].iov_base = iov[i].iov_base;
		rx_entry->liov[j].iov_len = iov[i].iov_len;
		rx_entry->total_len += iov[i].iov_len;
		zmr = desc[i];
		rx_entry->liov[j].iov_desc = zmr;
		if (!zmr)
			rx_entry->flags |= FI_INJECT;
		else {
			ret = zhpeq_lcl_key_access(
				zmr->kdata,
				rx_entry->liov[j].iov_base,
				rx_entry->liov[j].iov_len,
				ZHPEQ_MR_RECV, &rx_entry->liov[j].iov_zaddr);
			if (ret < 0)
				goto done;
		}
		j++;
		rx_entry->lstate.cnt = j;
	}

#if 0
	/* FIXME: Let's think about this some more. */
	if ((rx_entry->flags & FI_INJECT) &&
	    rx_entry->total_len > zhpe_ep_max_eager_sz) {
		ret = zhpe_mr_reg_int_oneshot(rx_ctx->domain, rx_entry->liov,
					      rx_entry->total_len, FI_RECV);
		if (ret < 0)
			goto done;
	}
#endif

	ret = 0;

	if (flags & FI_CLAIM) {
		rx_claimed = ((struct fi_context *)context)->internal[0];
		zhpe_pe_rx_claim_recv(rx_claimed, rx_entry);
		goto done;
	}

	zhpe_pe_rx_post_recv(rx_ctx, rx_entry);
	ZHPE_LOG_DBG("New rx_entry: %p (ctx: %p)\n", rx_entry, rx_ctx);
 done:
	if (ret < 0 && rx_entry) {
		zhpe_mr_close_oneshot(rx_entry->liov, rx_entry->lstate.cnt,
				      true);
		fastlock_acquire(&rx_ctx->lock);
		zhpe_rx_release_entry(rx_ctx, rx_entry);
		fastlock_release(&rx_ctx->lock);
	}

	return ret;
}

ssize_t zhpe_do_recvmsg(struct fid_ep *ep, const void *vmsg,
			uint64_t flags, bool tagged)
{
	/* Used by trigger: flags are assumed to be correct. */
	return do_recvmsg(ep, vmsg, flags, tagged);
}

static ssize_t zhpe_ep_recvmsg(struct fid_ep *ep, const struct fi_msg *msg,
			       uint64_t flags)
{
	if (flags & ZHPE_BAD_FLAGS_MASK)
		return -EINVAL;

	return do_recvmsg(ep, msg, flags, false);
}

static ssize_t zhpe_ep_recv(struct fid_ep *ep, void *buf, size_t len,
				void *desc, fi_addr_t src_addr, void *context)
{
	struct fi_msg msg;
	struct iovec msg_iov;
	memset(&msg, 0, sizeof(msg));
	msg_iov.iov_base = buf;
	msg_iov.iov_len = len;

	msg.msg_iov = &msg_iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = src_addr;
	msg.context = context;
	msg.data = 0;

	return do_recvmsg(ep, &msg, ZHPE_USE_OP_FLAGS, false);
}

static ssize_t zhpe_ep_recvv(struct fid_ep *ep, const struct iovec *iov,
		       void **desc, size_t count, fi_addr_t src_addr,
		       void *context)
{
	struct fi_msg msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = src_addr;
	msg.context = context;
	msg.data = 0;

	return do_recvmsg(ep, &msg, ZHPE_USE_OP_FLAGS, false);
}

static ssize_t do_sendmsg(struct fid_ep *ep, const void *vmsg, uint64_t flags,
			  bool tagged)
{
	ssize_t			ret = -FI_EINVAL;
	int64_t			tindex = -1;
	struct zhpe_msg_hdr	hdr = { .op_type = ZHPE_OP_SEND };
	struct zhpe_pe_entry	*pe_entry;
	size_t			inline_size;
	size_t			cmd_len;
	size_t			i;
	size_t			j;
	struct zhpe_msg_hdr	*zhdr;
	union zhpe_msg_payload	*zpay;
	uint64_t		lzaddr;
	const struct fi_msg	*msg;
	const struct fi_msg_tagged *tmsg;
	size_t			iov_count;
	const struct iovec	*iov;
	void			**desc;
	fi_addr_t		fiaddr;
	uint64_t		cq_data;
	uint64_t		tag;
	uint64_t		*data;
	uint64_t		op_flags;
	struct zhpe_conn	*conn;
	struct zhpe_tx_ctx	*tx_ctx;
	struct zhpe_ep		*zhpe_ep;
	struct zhpe_ep_attr	*ep_attr;
	struct zhpe_mr		*zmr;
	void			*context;

	ZHPEQ_TIMING_UPDATE(&zhpeq_timing_tx_start,
			    NULL, &zhpeq_timing_tx_start_stamp,
			    ZHPEQ_TIMING_UPDATE_OLD_CPU);

	switch (ep->fid.fclass) {
	case FI_CLASS_EP:
		zhpe_ep = container_of(ep, struct zhpe_ep, ep);
		ep_attr = zhpe_ep->attr;
		tx_ctx = (ep_attr->tx_ctx->use_shared ?
			  ep_attr->tx_ctx->stx_ctx :
			  ep_attr->tx_ctx);
		op_flags = zhpe_ep->tx_attr.op_flags;
		break;
	case FI_CLASS_TX_CTX:
		tx_ctx = container_of(ep, struct zhpe_tx_ctx, fid.ctx);
		ep_attr = tx_ctx->ep_attr;
		op_flags = tx_ctx->attr.op_flags;
		break;
	default:
		ZHPE_LOG_ERROR("Invalid EP type\n");
		goto done;
	}

	if (!tx_ctx->enabled) {
		ret = -FI_EOPBADSTATE;
		goto done;
	}

	if (likely(!(flags & ZHPE_TRIGGERED_OP))) {
		if (flags &
		    ~(ZHPE_NO_COMPLETION | ZHPE_USE_OP_FLAGS |
		      FI_COMPLETION | FI_TRIGGER | FI_INJECT |
		      FI_REMOTE_CQ_DATA | FI_INJECT_COMPLETE |
		      FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE | FI_FENCE |
		      FI_MSG | FI_SEND))
			goto done;
		flags |= (FI_MSG | FI_SEND);

		flags = zhpe_tx_fixup_completion(flags, op_flags, tx_ctx);
	}

	if (flags & FI_TRIGGER) {
		if (tagged)
			ret = zhpe_queue_tmsg_op(ep, vmsg, flags, FI_OP_TSEND);
		else
			ret = zhpe_queue_tmsg_op(ep, vmsg, flags, FI_OP_SEND);
		if (ret != 1)
			goto done;
	}

	inline_size = ZHPE_RING_ENTRY_LEN;

	if (flags & FI_REMOTE_CQ_DATA) {
		hdr.flags |= ZHPE_MSG_REMOTE_CQ_DATA;
		inline_size -= sizeof(cq_data);
	}
	if (tagged) {
		hdr.flags |= ZHPE_MSG_TAGGED;
		inline_size -= sizeof(tag);
		tmsg = vmsg;
		iov_count = tmsg->iov_count;
		iov = tmsg->msg_iov;
		desc = tmsg->desc;
		fiaddr = tmsg->addr;
		cq_data = tmsg->data;
		tag = tmsg->tag;
		context = tmsg->context;
	} else {
		msg = vmsg;
		iov_count = msg->iov_count;
		iov = msg->msg_iov;
		desc = msg->desc;
		fiaddr = msg->addr;
		cq_data = msg->data;
		tag = 0;
		context = msg->context;
	}

	ret = zhpe_ep_get_conn(ep_attr, fiaddr, &conn);
	if (ret < 0)
		goto done;

	ZHPE_LOG_DBG("New sendmsg on TX: %p using conn: %p\n",
		      tx_ctx, conn);

	/* FIXME: IOV > 1
	 * While some of the loops support iov size > 1, the
	 * fundamental protocol currently does not.
	 */
	zhpe_tx_reserve_vars(ret, zhpe_pe_tx_handle_entry, conn, context,
			     tindex, pe_entry, zhdr, lzaddr, done, 0);
	hdr.rx_id = zhpe_get_rx_id(tx_ctx, fiaddr);
	hdr.pe_entry_id = htons(tindex);

	/* FIXME: zhpe_ep_max_eager_sz  */
	inline_size -= conn->hdr_off + sizeof(*zhdr);

	pe_entry->rem = 0;
	for (i = 0, j = 0; i < iov_count; i++) {
		if (!iov[i].iov_len)
			continue;
		if (j >= ZHPE_EP_MAX_IOV_LIMIT) {
			ret = -FI_EINVAL;
			goto done;
		}
		pe_entry->ziov[j].iov_base = iov[i].iov_base;
		pe_entry->ziov[j].iov_len = iov[i].iov_len;
		pe_entry->rem += iov[i].iov_len;
		if (desc && (zmr = desc[i])) {
			pe_entry->ziov[j].iov_desc = zmr;
			ret = zhpeq_lcl_key_access(
				zmr->kdata, pe_entry->ziov[j].iov_base,
				pe_entry->ziov[j].iov_len,  ZHPEQ_MR_SEND,
				&pe_entry->ziov[j].iov_zaddr);
			if (ret < 0)
				goto done;
		} else
			pe_entry->ziov[j].iov_desc = NULL;
		j++;
		pe_entry->zstate.cnt = j;
	}

	/* Build TX command. */
	if (pe_entry->rem > inline_size) {
		ret = zhpe_mr_reg_int_oneshot(ep_attr->domain, pe_entry->ziov,
					      pe_entry->rem, FI_SEND);
		if (ret < 0)
			goto done;
		for (i = 0; i < pe_entry->zstate.cnt; i++) {
			ret = zhpe_conn_key_export(conn,
						   pe_entry->ziov[i].iov_desc,
						   false, hdr);
			if (ret < 0)
				goto done;
		}
		/* Align payload to uint64_t boundary. */
		zpay = zhpe_pay_ptr(conn, zhdr, 0, alignof(*zpay));
		zpay->indirect.tag = htobe64(tag);
		zpay->indirect.cq_data = htobe64(cq_data);
		zpay->indirect.vaddr =
			htobe64((uintptr_t)pe_entry->ziov[0].iov_base);
		zpay->indirect.len =
			htobe64((uintptr_t)pe_entry->ziov[0].iov_len);
		zpay->indirect.key =
			htobe64(pe_entry->ziov[0].iov_desc->mr_fid.key);
		cmd_len = zpay->indirect.end - (char *)zhdr;
		pe_entry->pe_root.completions++;
		if (flags & FI_DELIVERY_COMPLETE)
			hdr.flags |= ZHPE_MSG_DELIVERY_COMPLETE;
		else {
			hdr.flags |= ZHPE_MSG_TRANSMIT_COMPLETE;
			if (flags & FI_INJECT_COMPLETE) {
				flags &= ~FI_INJECT_COMPLETE;
				flags |= FI_TRANSMIT_COMPLETE;
			}
		}
	} else {
		hdr.flags |= ZHPE_MSG_INLINE;
		hdr.inline_len = pe_entry->rem;
		memcpy(zhpe_pay_ptr(conn, zhdr, 0, sizeof(int)),
		       iov[0].iov_base, pe_entry->rem);

		data = zhpe_pay_ptr(conn, zhdr, pe_entry->rem, alignof(*data));
		if (tagged)
			*data++ = htobe64(tag);
		if (hdr.flags & ZHPE_MSG_REMOTE_CQ_DATA)
			*data++ = htobe64(cq_data);
		cmd_len = (char *)data - (char *)zhdr;
		if (flags & FI_DELIVERY_COMPLETE) {
			hdr.flags |= ZHPE_MSG_DELIVERY_COMPLETE;
			pe_entry->pe_root.completions++;
		}
	}

	*zhdr = hdr;
	pe_entry->flags = flags;
	ret = zhpe_pe_tx_ring(pe_entry, zhdr, lzaddr, cmd_len);
 done:
	if (ret < 0 && tindex != -1) {
		zhpe_mr_close_oneshot(pe_entry->ziov, pe_entry->zstate.cnt,
				      true);
		zhpe_tx_release(conn->ztx, tindex, false);
	}

	return ret;
}

ssize_t zhpe_do_sendmsg(struct fid_ep *ep, const void *vmsg,
			uint64_t flags, bool tagged)
{
	/* Used by trigger: flags are assumed to be correct. */
	return do_sendmsg(ep, vmsg, flags, tagged);
}

static ssize_t zhpe_ep_sendmsg(struct fid_ep *ep, const struct fi_msg *msg,
			       uint64_t flags)
{
	if (flags & ZHPE_BAD_FLAGS_MASK)
		return -EINVAL;

	return do_sendmsg(ep, msg, flags, false);
}

static ssize_t zhpe_ep_send(struct fid_ep *ep, const void *buf, size_t len,
			    void *desc, fi_addr_t dest_addr, void *context)
{
	struct fi_msg msg;
	struct iovec msg_iov;
	memset(&msg, 0, sizeof(msg));
	msg_iov.iov_base = (void *) buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.context = context;

	return do_sendmsg(ep, &msg, ZHPE_USE_OP_FLAGS, false);
}

static ssize_t zhpe_ep_sendv(struct fid_ep *ep, const struct iovec *iov,
			     void **desc, size_t count, fi_addr_t dest_addr,
			     void *context)
{
	struct fi_msg msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = dest_addr;
	msg.context = context;

	return do_sendmsg(ep, &msg, ZHPE_USE_OP_FLAGS, false);
}

static ssize_t zhpe_ep_senddata(struct fid_ep *ep, const void *buf, size_t len,
				void *desc, uint64_t data, fi_addr_t dest_addr,
				void *context)
{
	struct fi_msg msg;
	struct iovec msg_iov;

	msg_iov.iov_base = (void *) buf;
	msg_iov.iov_len = len;

	msg.msg_iov = &msg_iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.context = context;
	msg.data = data;

	return do_sendmsg(ep, &msg, FI_REMOTE_CQ_DATA | ZHPE_USE_OP_FLAGS,
			  false);
}

static ssize_t zhpe_ep_inject(struct fid_ep *ep, const void *buf, size_t len,
			      fi_addr_t dest_addr)
{
	struct fi_msg msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof(msg));
	msg_iov.iov_base = (void *) buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;
	msg.iov_count = 1;
	msg.addr = dest_addr;

	return do_sendmsg(ep, &msg,
			  FI_INJECT | ZHPE_NO_COMPLETION | ZHPE_USE_OP_FLAGS,
			  false);
}

static ssize_t zhpe_ep_injectdata(struct fid_ep *ep, const void *buf,
				  size_t len, uint64_t data,
				  fi_addr_t dest_addr)
{
	struct fi_msg msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof(msg));
	msg_iov.iov_base = (void *) buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;

	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.data = data;

	return do_sendmsg(ep, &msg,
			  (FI_REMOTE_CQ_DATA | FI_INJECT |
			   ZHPE_NO_COMPLETION | ZHPE_USE_OP_FLAGS), false);
}

struct fi_ops_msg zhpe_ep_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = zhpe_ep_recv,
	.recvv = zhpe_ep_recvv,
	.recvmsg = zhpe_ep_recvmsg,
	.send = zhpe_ep_send,
	.sendv = zhpe_ep_sendv,
	.sendmsg = zhpe_ep_sendmsg,
	.inject = zhpe_ep_inject,
	.senddata = zhpe_ep_senddata,
	.injectdata = zhpe_ep_injectdata
};

static ssize_t zhpe_ep_trecvmsg(struct fid_ep *ep,
				const struct fi_msg_tagged *msg,
				uint64_t flags)
{
	if (flags & ZHPE_BAD_FLAGS_MASK)
		return -EINVAL;

	return do_recvmsg(ep, msg, flags, true);
}

static ssize_t zhpe_ep_trecv(struct fid_ep *ep, void *buf, size_t len,
			     void *desc, fi_addr_t src_addr, uint64_t tag,
			     uint64_t ignore, void *context)
{
	struct fi_msg_tagged msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof(msg));
	msg_iov.iov_base = buf;
	msg_iov.iov_len = len;

	msg.msg_iov = &msg_iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = src_addr;
	msg.context = context;
	msg.tag = tag;
	msg.ignore = ignore;
	msg.data = 0;

	return do_recvmsg(ep, &msg, ZHPE_USE_OP_FLAGS, true);
}

static ssize_t zhpe_ep_trecvv(struct fid_ep *ep, const struct iovec *iov,
			       void **desc, size_t count, fi_addr_t src_addr,
			       uint64_t tag, uint64_t ignore, void *context)
{
	struct fi_msg_tagged msg;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = src_addr;
	msg.context = context;
	msg.tag = tag;
	msg.ignore = ignore;
	msg.data = 0;

	return do_recvmsg(ep, &msg, ZHPE_USE_OP_FLAGS, true);
}

static ssize_t zhpe_ep_tsendmsg(struct fid_ep *ep,
				const struct fi_msg_tagged *msg,
				uint64_t flags)
{
	if (flags & ZHPE_BAD_FLAGS_MASK)
		return -EINVAL;

	return do_sendmsg(ep, msg, flags, true);
}

static ssize_t zhpe_ep_tsend(struct fid_ep *ep, const void *buf, size_t len,
			     void *desc, fi_addr_t dest_addr, uint64_t tag,
			     void *context)
{
	struct fi_msg_tagged msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof(msg));
	msg_iov.iov_base = (void *) buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.context = context;
	msg.tag = tag;

	return do_sendmsg(ep, &msg, ZHPE_USE_OP_FLAGS, true);
}

static ssize_t zhpe_ep_tsendv(struct fid_ep *ep, const struct iovec *iov,
			      void **desc, size_t count, fi_addr_t dest_addr,
			      uint64_t tag, void *context)
{
	struct fi_msg_tagged msg;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = dest_addr;
	msg.context = context;
	msg.tag = tag;

	return do_sendmsg(ep, &msg, ZHPE_USE_OP_FLAGS, true);
}

static ssize_t zhpe_ep_tsenddata(struct fid_ep *ep, const void *buf, size_t len,
				 void *desc, uint64_t data, fi_addr_t dest_addr,
				 uint64_t tag, void *context)
{
	struct fi_msg_tagged msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof(msg));
	msg_iov.iov_base = (void *) buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;
	msg.desc = desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.context = context;
	msg.data = data;
	msg.tag = tag;

	return do_sendmsg(ep, &msg, FI_REMOTE_CQ_DATA | ZHPE_USE_OP_FLAGS,
			  true);
}

static ssize_t zhpe_ep_tinject(struct fid_ep *ep, const void *buf, size_t len,
				fi_addr_t dest_addr, uint64_t tag)
{
	struct fi_msg_tagged msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof(msg));
	msg_iov.iov_base = (void *) buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.tag = tag;

	return do_sendmsg(ep, &msg,
			  FI_INJECT | ZHPE_NO_COMPLETION | ZHPE_USE_OP_FLAGS,
			  true);
}

static ssize_t zhpe_ep_tinjectdata(struct fid_ep *ep, const void *buf,
				   size_t len, uint64_t data,
				   fi_addr_t dest_addr, uint64_t tag)
{
	struct fi_msg_tagged msg;
	struct iovec msg_iov;

	memset(&msg, 0, sizeof(msg));
	msg_iov.iov_base = (void *) buf;
	msg_iov.iov_len = len;
	msg.msg_iov = &msg_iov;

	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.data = data;
	msg.tag = tag;

	return do_sendmsg(ep, &msg,
			  (FI_REMOTE_CQ_DATA | FI_INJECT |
			   ZHPE_NO_COMPLETION | ZHPE_USE_OP_FLAGS), true);
}


struct fi_ops_tagged zhpe_ep_tagged = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = zhpe_ep_trecv,
	.recvv = zhpe_ep_trecvv,
	.recvmsg = zhpe_ep_trecvmsg,
	.send = zhpe_ep_tsend,
	.sendv = zhpe_ep_tsendv,
	.sendmsg = zhpe_ep_tsendmsg,
	.inject = zhpe_ep_tinject,
	.senddata = zhpe_ep_tsenddata,
	.injectdata = zhpe_ep_tinjectdata,
};

