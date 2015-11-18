/*
 * Copyright (c) 2014, Cisco Systems, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <asm/types.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_prov.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_errno.h>
#include "fi.h"
#include "fi_enosys.h"

#include "usnic_direct.h"
#include "usd.h"
#include "usdf.h"
#include "usdf_av.h"
#include "usdf_progress.h"
#include "usdf_cq.h"

static inline int usdf_cqe_to_flags(struct usd_completion *comp)
{
	switch (comp->uc_type) {
	case USD_COMPTYPE_SEND:
		return (FI_MSG | FI_SEND);
	case USD_COMPTYPE_RECV:
		return (FI_MSG | FI_RECV);
	default:
		USDF_DBG_SYS(CQ, "WARNING: unknown completion type! (%d)\n",
				comp->uc_type);
		return 0;
	}

}

static ssize_t
usdf_cq_readerr(struct fid_cq *fcq, struct fi_cq_err_entry *entry,
	        uint64_t flags)
{
	struct usdf_cq *cq;

	USDF_TRACE_SYS(CQ, "\n");

	cq = container_of(fcq, struct usdf_cq, cq_fid);

	// The return values are analogous to sockets cq_readerr
	if (cq->cq_comp.uc_status == 0) {
		return -FI_EAGAIN;
	}

	entry->op_context = cq->cq_comp.uc_context;
	entry->flags = 0;
	entry->err = FI_EIO;
	switch (cq->cq_comp.uc_status) {
	case USD_COMPSTAT_SUCCESS:
		entry->prov_errno = FI_SUCCESS;
		break;
	case USD_COMPSTAT_ERROR_CRC:
		entry->prov_errno = FI_ECRC;
		break;
	case USD_COMPSTAT_ERROR_TRUNC:
		entry->prov_errno = FI_ETRUNC;
		break;
	case USD_COMPSTAT_ERROR_TIMEOUT:
		entry->prov_errno = FI_ETIMEDOUT;
		break;
	case USD_COMPSTAT_ERROR_INTERNAL:
		entry->prov_errno = FI_EOTHER;
		break;
	}

	cq->cq_comp.uc_status = 0;

	return 1;
}

static ssize_t
usdf_cq_readerr_soft(struct fid_cq *fcq, struct fi_cq_err_entry *entry,
		uint64_t flags)
{
	struct usdf_cq *cq;
	struct usdf_cq_soft_entry *tail;

	USDF_TRACE_SYS(CQ, "\n");

	cq = container_of(fcq, struct usdf_cq, cq_fid);

	tail = cq->c.soft.cq_tail;

	entry->op_context = tail->cse_context;
	entry->flags = 0;
	entry->err = FI_EIO;
	entry->prov_errno = tail->cse_prov_errno;

	tail++;
	if (tail == cq->c.soft.cq_end) {
		tail = cq->c.soft.cq_comps;
	}
	cq->c.soft.cq_tail = tail;

	return 1;
}

/* Completion lengths should reflect the length given by the application to the
 * send/recv call. This means we need to update the lengths for both prefix and
 * non-prefix send paths.
 *
 * RECEIVE COMPLETIONS
 *
 * Non-prefix: the application isn't aware of the usd_udp_hdr struct. Default
 * completion semantics include this in the completion length since it is part
 * of the send.
 *
 * Prefix: the application has allocated a buffer that includes the advertised
 * prefix size. For performance reasons our advertised prefix size is not the
 * same size as hour headers. To reflect the correct size we need to add the
 * size of the padding.
 *
 * SEND COMPLETIONS
 * The send completions are dependent upon the wp_len value that is set by the
 * library when using the underscore prefixed variants of the usd functions or
 * by the usd library when using the non-underscore prefixed variants.
 * Currently all send functions have been unified to report wp_len as the
 * length of the payload. This means that adjustments need to be made when in
 * libfabric prefix mode.
 */
static inline void usdf_cq_adjust_len(struct usd_completion *src,
		size_t *len)
{
	struct usdf_ep *ep = src->uc_qp->uq_context;

	if (src->uc_type == USD_COMPTYPE_RECV) {
		if (ep->ep_mode & FI_MSG_PREFIX)
			*len += (USDF_HDR_BUF_ENTRY -
					sizeof(struct usd_udp_hdr));
		else
			*len -= sizeof(struct usd_udp_hdr);
	} else {
		if (ep->ep_mode & FI_MSG_PREFIX)
			*len += USDF_HDR_BUF_ENTRY;
	}
}

static inline ssize_t
usdf_cq_copy_cq_entry(void *dst, struct usd_completion *src,
			enum fi_cq_format format)
{
	struct fi_cq_entry *ctx_entry;
	struct fi_cq_msg_entry *msg_entry;
	struct fi_cq_data_entry *data_entry;

	switch (format) {
	case FI_CQ_FORMAT_CONTEXT:
		ctx_entry = (struct fi_cq_entry *)dst;
		ctx_entry->op_context = src->uc_context;
		break;
	case FI_CQ_FORMAT_MSG:
		msg_entry = (struct fi_cq_msg_entry *)dst;
		msg_entry->op_context = src->uc_context;
		msg_entry->flags = usdf_cqe_to_flags(src);
		msg_entry->len = src->uc_bytes;

		usdf_cq_adjust_len(src, &msg_entry->len);

		break;
	case FI_CQ_FORMAT_DATA:
		data_entry = (struct fi_cq_data_entry *)dst;
		data_entry->op_context = src->uc_context;
		data_entry->flags = usdf_cqe_to_flags(src);
		data_entry->len = src->uc_bytes;
		data_entry->buf = 0; /* XXX */
		data_entry->data = 0;

		usdf_cq_adjust_len(src, &data_entry->len);

		break;
	default:
		USDF_WARN("unexpected CQ format, internal error\n");
		return -FI_EOPNOTSUPP;
	}

	return FI_SUCCESS;
}

static inline ssize_t
usdf_cq_sread_common(struct fid_cq *fcq, void *buf, size_t count, const void *cond,
			int timeout_ms, enum fi_cq_format format)
{
	struct usdf_cq *cq;
	uint8_t *entry;
	uint8_t *last;
	size_t entry_len;
	ssize_t ret;
	size_t sleep_time_us;
	size_t time_spent_us = 0;

	sleep_time_us = SREAD_INIT_SLEEP_TIME_US;

	cq = cq_ftou(fcq);
	if (cq->cq_comp.uc_status != 0)
		return -FI_EAVAIL;

	switch (format) {
	case FI_CQ_FORMAT_CONTEXT:
		entry_len = sizeof(struct fi_cq_entry);
		break;
	case FI_CQ_FORMAT_MSG:
		entry_len = sizeof(struct fi_cq_msg_entry);
		break;
	case FI_CQ_FORMAT_DATA:
		entry_len = sizeof(struct fi_cq_data_entry);
		break;
	default:
		return 0;
	}

	ret = 0;
	entry = buf;
	last = entry + (entry_len * count);

	while (entry < last) {
		ret = usd_poll_cq(cq->c.hard.cq_cq, &cq->cq_comp);
		if (ret == -EAGAIN) {
			if (entry > (uint8_t *)buf)
				break;
			if (timeout_ms >= 0 &&
				(time_spent_us >= 1000 * timeout_ms))
				break;

			usleep(sleep_time_us);
			time_spent_us += sleep_time_us;

			/* exponentially back off up to a limit */
			if (sleep_time_us < SREAD_MAX_SLEEP_TIME_US)
				sleep_time_us *= SREAD_EXP_BASE;
			sleep_time_us = MIN(sleep_time_us,
						SREAD_MAX_SLEEP_TIME_US);

			continue;
		}
		if (cq->cq_comp.uc_status != 0) {
			if (entry > (uint8_t *) buf)
				break;
			else
				return -FI_EAVAIL;
		}

		ret = usdf_cq_copy_cq_entry(entry, &cq->cq_comp, format);
		if (ret < 0)
			return ret;

		entry += entry_len;
	}

	if (entry > (uint8_t *)buf)
		return (entry - (uint8_t *)buf) / entry_len;
	return -FI_EAGAIN;
}

static ssize_t
usdf_cq_sread_context(struct fid_cq *fcq, void *buf, size_t count,
			const void *cond, int timeout)
{
	return usdf_cq_sread_common(fcq, buf, count, cond, timeout, 
					FI_CQ_FORMAT_CONTEXT);
}

static ssize_t
usdf_cq_sread_msg(struct fid_cq *fcq, void *buf, size_t count,
			const void *cond, int timeout)
{
	return usdf_cq_sread_common(fcq, buf, count, cond, timeout,
					FI_CQ_FORMAT_MSG);
}

static ssize_t
usdf_cq_sread_data(struct fid_cq *fcq, void *buf, size_t count,
			const void *cond, int timeout)
{
	return usdf_cq_sread_common(fcq, buf, count, cond, timeout,
					FI_CQ_FORMAT_DATA);
}

/*
 * poll a hard CQ
 * Since this routine is an inline and is always called with format as
 * a constant, I am counting on the compiler optimizing away all the switches
 * on format.
 */
static inline ssize_t
usdf_cq_read_common(struct fid_cq *fcq, void *buf, size_t count,
		enum fi_cq_format format)
{
	struct usdf_cq *cq;
	uint8_t *entry;
	uint8_t *last;
	size_t entry_len;
	ssize_t ret;

	cq = cq_ftou(fcq);
	if (cq->cq_comp.uc_status != 0)
		return -FI_EAVAIL;

	switch (format) {
	case FI_CQ_FORMAT_CONTEXT:
		entry_len = sizeof(struct fi_cq_entry);
		break;
	case FI_CQ_FORMAT_MSG:
		entry_len = sizeof(struct fi_cq_msg_entry);
		break;
	case FI_CQ_FORMAT_DATA:
		entry_len = sizeof(struct fi_cq_data_entry);
		break;
	default:
		return 0;
	}

	ret = 0;
	entry = buf;
	last = entry + (entry_len * count);

	while (entry < last) {
		ret = usd_poll_cq(cq->c.hard.cq_cq, &cq->cq_comp);
		if (ret == -EAGAIN)
			break;
		if (cq->cq_comp.uc_status != 0) {
			ret = -FI_EAVAIL;
			break;
		}
		ret = usdf_cq_copy_cq_entry(entry, &cq->cq_comp, format);
		if (ret < 0)
			return ret;
		entry += entry_len;
	}

	if (entry > (uint8_t *)buf)
		return (entry - (uint8_t *)buf) / entry_len;
	else
		return ret;
}

static ssize_t
usdf_cq_read_context(struct fid_cq *fcq, void *buf, size_t count)
{
	return usdf_cq_read_common(fcq, buf, count, FI_CQ_FORMAT_CONTEXT);
}

static ssize_t
usdf_cq_read_msg(struct fid_cq *fcq, void *buf, size_t count)
{
	return usdf_cq_read_common(fcq, buf, count, FI_CQ_FORMAT_MSG);
}

static ssize_t
usdf_cq_read_data(struct fid_cq *fcq, void *buf, size_t count)
{
	return usdf_cq_read_common(fcq, buf, count, FI_CQ_FORMAT_DATA);
}

static ssize_t
usdf_cq_readfrom_context(struct fid_cq *fcq, void *buf, size_t count,
			fi_addr_t *src_addr)
{
	struct usdf_cq *cq;
	struct usd_cq_impl *ucq;
	struct fi_cq_entry *entry;
	struct fi_cq_entry *last;
	ssize_t ret;
	struct cq_desc *cq_desc;
	struct usdf_ep *ep;
	struct sockaddr_in sin;
	struct usd_udp_hdr *hdr;
	uint16_t index;

	cq = cq_ftou(fcq);
	if (cq->cq_comp.uc_status != 0) {
		return -FI_EAVAIL;
	}
	ucq = to_cqi(cq->c.hard.cq_cq);

	ret = 0;
	entry = buf;
	last = entry + count;
	while (entry < last) {
		cq_desc = (struct cq_desc *)((uint8_t *)ucq->ucq_desc_ring +
				(ucq->ucq_next_desc << 4));

		ret = usd_poll_cq(cq->c.hard.cq_cq, &cq->cq_comp);
		if (ret == -EAGAIN) {
			ret = 0;
			break;
		}
		if (cq->cq_comp.uc_status != 0) {
			ret = -FI_EAVAIL;
			break;
		}

		if (cq->cq_comp.uc_type == USD_COMPTYPE_RECV) {
			index = le16_to_cpu(cq_desc->completed_index) &
				CQ_DESC_COMP_NDX_MASK;
			ep = cq->cq_comp.uc_qp->uq_context;
			hdr = ep->e.dg.ep_hdr_ptr[index];
			memset(&sin, 0, sizeof(sin));

			sin.sin_addr.s_addr = hdr->uh_ip.saddr;
			sin.sin_port = hdr->uh_udp.source;

			ret = fi_av_insert(av_utof(ep->e.dg.ep_av), &sin, 1,
					src_addr, 0, NULL);
			if (ret != 1) {
				*src_addr = FI_ADDR_NOTAVAIL;
			}
			++src_addr;
		}
			

		entry->op_context = cq->cq_comp.uc_context;

		entry++;
	}

	if (entry > (struct fi_cq_entry *)buf) {
		return entry - (struct fi_cq_entry *)buf;
	} else {
		return ret;
	}
}

/*****************************************************************
 * "soft" CQ support
 *****************************************************************/

void
usdf_progress_hard_cq(struct usdf_cq_hard *hcq)
{
	int ret;
	struct usd_completion comp;
	struct usdf_cq_soft_entry *entry;
	struct usdf_cq *cq;

	cq = hcq->cqh_cq;

	do {
		ret = usd_poll_cq(hcq->cqh_ucq, &comp);
		if (ret == 0) {
			entry = cq->c.soft.cq_head;

			/* If the current entry is equal to the tail and the
			 * last operation was a write, then we have filled the
			 * queue and we just drop whatever there isn't space
			 * for.
			 */
			if ((entry == cq->c.soft.cq_tail) &&
					(cq->c.soft.cq_last_op ==
						USDF_SOFT_CQ_WRITE))
				return;

			entry->cse_context = cq->cq_comp.uc_context;
			entry->cse_flags = 0;
			entry->cse_len = cq->cq_comp.uc_bytes;
			entry->cse_buf = 0;		 /* XXX TODO */
			entry->cse_data = 0;

			/* update with wrap */
			entry++;
			if (entry != cq->c.soft.cq_end) {
				cq->c.soft.cq_head = entry;
			} else {
				cq->c.soft.cq_head = cq->c.soft.cq_comps;
			}

			cq->c.soft.cq_last_op = USDF_SOFT_CQ_WRITE;
		}
	} while (ret != -EAGAIN);
}

void
usdf_cq_post_soft(struct usdf_cq_hard *hcq, void *context, size_t len,
		int prov_errno)
{
	struct usdf_cq_soft_entry *entry;
	struct usdf_cq *cq;

	cq = hcq->cqh_cq;

	entry = cq->c.soft.cq_head;

	/* If the current entry is equal to the tail and the
	 * last operation was a write, then we have filled the
	 * queue and we just drop whatever there isn't space
	 * for.
	 */
	if ((entry == cq->c.soft.cq_tail) &&
			(cq->c.soft.cq_last_op == USDF_SOFT_CQ_WRITE))
		return;

	entry->cse_context = context;
	entry->cse_len = len;
	entry->cse_prov_errno = prov_errno;

	/* update with wrap */
	entry++;
	if (entry != cq->c.soft.cq_end) {
		cq->c.soft.cq_head = entry;
	} else {
		cq->c.soft.cq_head = cq->c.soft.cq_comps;
	}

	cq->c.soft.cq_last_op = USDF_SOFT_CQ_WRITE;
}

static inline ssize_t
usdf_cq_copy_soft_entry(void *dst, const struct usdf_cq_soft_entry *src,
			enum fi_cq_format dst_format)
{
	struct fi_cq_entry *ctx_entry;
	struct fi_cq_msg_entry *msg_entry;
	struct fi_cq_data_entry *data_entry;

	switch (dst_format) {
	case FI_CQ_FORMAT_CONTEXT:
		ctx_entry = (struct fi_cq_entry *)dst;
		ctx_entry->op_context = src->cse_context;
		break;
	case FI_CQ_FORMAT_MSG:
		msg_entry = (struct fi_cq_msg_entry *)dst;
		msg_entry->op_context = src->cse_context;
		msg_entry->flags = src->cse_flags;
		msg_entry->len = src->cse_len;
		break;
	case FI_CQ_FORMAT_DATA:
		data_entry = (struct fi_cq_data_entry *)dst;
		data_entry->op_context = src->cse_context;
		data_entry->flags = src->cse_flags;
		data_entry->len = src->cse_len;
		data_entry->buf = src->cse_buf;
		data_entry->data = src->cse_data;
		break;
	default:
		USDF_WARN("unexpected CQ format, internal error\n");
		return -FI_EOPNOTSUPP;
	}

	return FI_SUCCESS;
}

static ssize_t
usdf_cq_sread_common_soft(struct fid_cq *fcq, void *buf, size_t count, const void *cond,
		int timeout_ms, enum fi_cq_format format)
{
	struct usdf_cq *cq;
	uint8_t *entry;
	uint8_t *last;
	struct usdf_cq_soft_entry *tail;
	size_t entry_len;
	size_t sleep_time_us;
	size_t time_spent_us = 0;
	ssize_t ret;

	cq = cq_ftou(fcq);
	sleep_time_us = SREAD_INIT_SLEEP_TIME_US;

	switch (format) {
	case FI_CQ_FORMAT_CONTEXT:
		entry_len = sizeof(struct fi_cq_entry);
		break;
	case FI_CQ_FORMAT_MSG:
		entry_len = sizeof(struct fi_cq_msg_entry);
		break;
	case FI_CQ_FORMAT_DATA:
		entry_len = sizeof(struct fi_cq_data_entry);
		break;
	default:
		USDF_WARN("unexpected CQ format, internal error\n");
		return -FI_EOPNOTSUPP;
	}

	entry = buf;
	last = entry + (entry_len * count);

	while (1) {
		/* progress... */
		usdf_domain_progress(cq->cq_domain);

		tail = cq->c.soft.cq_tail;

		while (entry < last) {
			/* If the head and tail are equal and the last
			 * operation was a read then that means we have an
			 * empty queue.
			 */
			if ((tail == cq->c.soft.cq_head) &&
					(cq->c.soft.cq_last_op ==
					 USDF_SOFT_CQ_READ))
				break;

			if (tail->cse_prov_errno > 0) {
				if (entry > (uint8_t *)buf)
					break;
				else
					return -FI_EAVAIL;
			}

			ret = usdf_cq_copy_soft_entry(entry, tail, format);
			if (ret < 0)
				return ret;

			entry += entry_len;
			tail++;
			if (tail == cq->c.soft.cq_end)
				tail = cq->c.soft.cq_comps;

			cq->c.soft.cq_last_op = USDF_SOFT_CQ_READ;
		}

		if (entry > (uint8_t *)buf) {
			cq->c.soft.cq_tail = tail;
			return (entry - (uint8_t *)buf) / entry_len;
		} else {
			if (timeout_ms >= 0 &&
				(time_spent_us >= 1000 * timeout_ms))
				break;

			usleep(sleep_time_us);
			time_spent_us += sleep_time_us;

			/* exponentially back off up to a limit */
			if (sleep_time_us < SREAD_MAX_SLEEP_TIME_US)
				sleep_time_us *= SREAD_EXP_BASE;
			sleep_time_us = MIN(sleep_time_us,
						SREAD_MAX_SLEEP_TIME_US);
		}
	}

	return -FI_EAGAIN;
}

static ssize_t
usdf_cq_sread_context_soft(struct fid_cq *fcq, void *buf, size_t count,
		const void *cond, int timeout)
{
	return usdf_cq_sread_common_soft(fcq, buf, count, cond, timeout,
					FI_CQ_FORMAT_CONTEXT);
}

static ssize_t
usdf_cq_sread_msg_soft(struct fid_cq *fcq, void *buf, size_t count,
		const void *cond, int timeout)
{
	return usdf_cq_sread_common_soft(fcq, buf, count, cond, timeout,
					FI_CQ_FORMAT_MSG);
}

static ssize_t
usdf_cq_sread_data_soft(struct fid_cq *fcq, void *buf, size_t count,
		const void *cond, int timeout)
{
	return usdf_cq_sread_common_soft(fcq, buf, count, cond, timeout,
					FI_CQ_FORMAT_DATA);
}

/*
 * poll a soft CQ
 * This will loop over all the hard CQs within, collecting results.
 * Since this routine is an inline and is always called with format as
 * a constant, I am counting on the compiler optimizing away all the switches
 * on format.
 */
static inline ssize_t
usdf_cq_read_common_soft(struct fid_cq *fcq, void *buf, size_t count,
		enum fi_cq_format format)
{
	struct usdf_cq *cq;
	uint8_t *entry;
	uint8_t *last;
	struct usdf_cq_soft_entry *tail;
	size_t entry_len;
	ssize_t ret;

	cq = cq_ftou(fcq);
	if (cq->cq_comp.uc_status != 0) {
		return -FI_EAVAIL;
	}

	/* progress... */
	usdf_domain_progress(cq->cq_domain);

	switch (format) {
	case FI_CQ_FORMAT_CONTEXT:
		entry_len = sizeof(struct fi_cq_entry);
		break;
	case FI_CQ_FORMAT_MSG:
		entry_len = sizeof(struct fi_cq_msg_entry);
		break;
	case FI_CQ_FORMAT_DATA:
		entry_len = sizeof(struct fi_cq_data_entry);
		break;
	default:
		USDF_WARN("unexpected CQ format, internal error\n");
		return -FI_EOPNOTSUPP;
	}

	entry = buf;
	last = entry + (entry_len * count);
	tail = cq->c.soft.cq_tail;

	while (entry < last) {
		/* If the head and tail are equal and the last
		 * operation was a read then that means we have an
		 * empty queue.
		 */
		if ((tail == cq->c.soft.cq_head) &&
				(cq->c.soft.cq_last_op == USDF_SOFT_CQ_READ))
			break;

		if (tail->cse_prov_errno > 0) {
			if (entry > (uint8_t *) buf)
				break;
			else
				return -FI_EAVAIL;
		}
		ret = usdf_cq_copy_soft_entry(entry, tail, format);
		if (ret < 0) {
			return ret;
		}
		entry += entry_len;
		tail++;
		if (tail == cq->c.soft.cq_end) {
			tail = cq->c.soft.cq_comps;
		}

		cq->c.soft.cq_last_op = USDF_SOFT_CQ_READ;
	}
	cq->c.soft.cq_tail = tail;

	if (entry > (uint8_t *)buf) {
		return (entry - (uint8_t *)buf) / entry_len;
	} else {
		return -FI_EAGAIN;
	}
}

static ssize_t
usdf_cq_read_context_soft(struct fid_cq *fcq, void *buf, size_t count)
{
	return usdf_cq_read_common_soft(fcq, buf, count, FI_CQ_FORMAT_CONTEXT);
}

static ssize_t
usdf_cq_read_msg_soft(struct fid_cq *fcq, void *buf, size_t count)
{
	return usdf_cq_read_common_soft(fcq, buf, count, FI_CQ_FORMAT_MSG);
}

static ssize_t
usdf_cq_read_data_soft(struct fid_cq *fcq, void *buf, size_t count)
{
	return usdf_cq_read_common_soft(fcq, buf, count, FI_CQ_FORMAT_DATA);
}

static ssize_t
usdf_cq_readfrom_context_soft(struct fid_cq *fcq, void *buf, size_t count,
			fi_addr_t *src_addr)
{
	struct usdf_cq *cq;
	struct usd_cq_impl *ucq;
	struct fi_cq_entry *entry;
	struct fi_cq_entry *last;
	ssize_t ret;
	struct cq_desc *cq_desc;
	struct usdf_ep *ep;
	struct sockaddr_in sin;
	struct usd_udp_hdr *hdr;
	uint16_t index;

	cq = cq_ftou(fcq);
	if (cq->cq_comp.uc_status != 0) {
		return -FI_EAVAIL;
	}
	ucq = to_cqi(cq->c.hard.cq_cq);

	ret = 0;
	entry = buf;
	last = entry + count;
	while (entry < last) {
		cq_desc = (struct cq_desc *)((uint8_t *)ucq->ucq_desc_ring +
				(ucq->ucq_next_desc << 4));

		ret = usd_poll_cq(cq->c.hard.cq_cq, &cq->cq_comp);
		if (ret == -EAGAIN) {
			ret = 0;
			break;
		}
		if (cq->cq_comp.uc_status != 0) {
			ret = -FI_EAVAIL;
			break;
		}

		if (cq->cq_comp.uc_type == USD_COMPTYPE_RECV) {
			index = le16_to_cpu(cq_desc->completed_index) &
				CQ_DESC_COMP_NDX_MASK;
			ep = cq->cq_comp.uc_qp->uq_context;
			hdr = ep->e.dg.ep_hdr_ptr[index];
			memset(&sin, 0, sizeof(sin));

			sin.sin_addr.s_addr = hdr->uh_ip.saddr;
			sin.sin_port = hdr->uh_udp.source;

			ret = fi_av_insert(av_utof(ep->e.dg.ep_av), &sin, 1,
					src_addr, 0, NULL);
			if (ret != 1) {
				*src_addr = FI_ADDR_NOTAVAIL;
			}
			++src_addr;
		}
			

		entry->op_context = cq->cq_comp.uc_context;

		entry++;
	}

	if (entry > (struct fi_cq_entry *)buf) {
		return entry - (struct fi_cq_entry *)buf;
	} else {
		return ret;
	}
}

/*****************************************************************
 * common CQ support
 *****************************************************************/

static const char *
usdf_cq_strerror(struct fid_cq *eq, int prov_errno, const void *err_data,
		 char *buf, size_t len)
{
	if (buf && len) {
		strncpy(buf, fi_strerror(prov_errno), len);
		buf[len-1] = '\0';
		return buf;
	}
	return fi_strerror(prov_errno);
}

static int
usdf_cq_control(fid_t fid, int command, void *arg)
{
	USDF_TRACE_SYS(CQ, "\n");
	return -FI_ENOSYS;
}

static int
usdf_cq_close(fid_t fid)
{
	struct usdf_cq *cq;
	struct usdf_cq_hard *hcq;
	int ret;

	USDF_TRACE_SYS(CQ, "\n");

	cq = container_of(fid, struct usdf_cq, cq_fid.fid);
	if (atomic_get(&cq->cq_refcnt) > 0) {
		return -FI_EBUSY;
	}

	if (usdf_cq_is_soft(cq)) {
		while (!TAILQ_EMPTY(&cq->c.soft.cq_list)) {
			hcq = TAILQ_FIRST(&cq->c.soft.cq_list);
			if (atomic_get(&hcq->cqh_refcnt) > 0) {
				return -FI_EBUSY;
			}
			TAILQ_REMOVE(&cq->c.soft.cq_list, hcq, cqh_link);
			if (hcq->cqh_ucq != NULL) {
				ret = usd_destroy_cq(hcq->cqh_ucq);
				if (ret != 0) {
					return ret;
				}
			}
			free(hcq);
		}
	} else {
		if (cq->c.hard.cq_cq) {
			ret = usd_destroy_cq(cq->c.hard.cq_cq);
			if (ret != 0) {
				return ret;
			}
		}
	}

	free(cq);
	return 0;
}

static struct fi_ops_cq usdf_cq_context_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = usdf_cq_read_context,
	.readfrom = usdf_cq_readfrom_context,
	.readerr = usdf_cq_readerr,
	.sread = usdf_cq_sread_context,
	.sreadfrom = fi_no_cq_sreadfrom,
	.signal = fi_no_cq_signal,
	.strerror = usdf_cq_strerror,
};

static struct fi_ops_cq usdf_cq_context_soft_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = usdf_cq_read_context_soft,
	.readfrom = usdf_cq_readfrom_context_soft,
	.readerr = usdf_cq_readerr_soft,
	.sread = usdf_cq_sread_context_soft,
	.sreadfrom = fi_no_cq_sreadfrom,
	.signal = fi_no_cq_signal,
	.strerror = usdf_cq_strerror,
};

static struct fi_ops_cq usdf_cq_msg_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = usdf_cq_read_msg,
	.readfrom = fi_no_cq_readfrom,  /* XXX */
	.readerr = usdf_cq_readerr,
	.sread = usdf_cq_sread_msg,
	.sreadfrom = fi_no_cq_sreadfrom,
	.signal = fi_no_cq_signal,
	.strerror = usdf_cq_strerror,
};

static struct fi_ops_cq usdf_cq_msg_soft_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = usdf_cq_read_msg_soft,
	.readfrom = fi_no_cq_readfrom,  /* XXX */
	.readerr = usdf_cq_readerr_soft,
	.sread = usdf_cq_sread_msg_soft,
	.sreadfrom = fi_no_cq_sreadfrom,
	.signal = fi_no_cq_signal,
	.strerror = usdf_cq_strerror,
};

static struct fi_ops_cq usdf_cq_data_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = usdf_cq_read_data,
	.readfrom = fi_no_cq_readfrom,  /* XXX */
	.readerr = usdf_cq_readerr,
	.sread = usdf_cq_sread_data,
	.sreadfrom = fi_no_cq_sreadfrom,
	.signal = fi_no_cq_signal,
	.strerror = usdf_cq_strerror,
};

static struct fi_ops_cq usdf_cq_data_soft_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = usdf_cq_read_data_soft,
	.readfrom = fi_no_cq_readfrom,  /* XXX */
	.readerr = usdf_cq_readerr_soft,
	.sread = usdf_cq_sread_data_soft,
	.sreadfrom = fi_no_cq_sreadfrom,
	.signal = fi_no_cq_signal,
	.strerror = usdf_cq_strerror,
};

static struct fi_ops usdf_cq_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = usdf_cq_close,
	.bind = fi_no_bind,
	.control = usdf_cq_control,
	.ops_open = fi_no_ops_open,
};

/*
 * Return true is this CQ is in "soft" (emulated) mode
 */
int
usdf_cq_is_soft(struct usdf_cq *cq)
{
	struct fi_ops_cq *soft_ops;

        switch (cq->cq_attr.format) {
        case FI_CQ_FORMAT_CONTEXT:
                soft_ops = &usdf_cq_context_soft_ops;
                break;
        case FI_CQ_FORMAT_MSG:
                soft_ops = &usdf_cq_msg_soft_ops;
                break;
        case FI_CQ_FORMAT_DATA:
                soft_ops = &usdf_cq_data_soft_ops;
                break;
	default:
		return 0;
        }

	return cq->cq_fid.ops == soft_ops;
}

int
usdf_cq_make_soft(struct usdf_cq *cq)
{
        struct fi_ops_cq *hard_ops;
        struct fi_ops_cq *soft_ops;
	struct usdf_cq_hard *hcq;
	struct usd_cq *ucq;
	void (*rtn)(struct usdf_cq_hard *hcq);

        switch (cq->cq_attr.format) {
        case FI_CQ_FORMAT_CONTEXT:
                hard_ops = &usdf_cq_context_ops;
                soft_ops = &usdf_cq_context_soft_ops;
                break;
        case FI_CQ_FORMAT_MSG:
                hard_ops = &usdf_cq_msg_ops;
                soft_ops = &usdf_cq_msg_soft_ops;
                break;
        case FI_CQ_FORMAT_DATA:
                hard_ops = &usdf_cq_data_ops;
                soft_ops = &usdf_cq_data_soft_ops;
                break;
	default:
		return 0;
        }

	rtn = usdf_progress_hard_cq;

        if (cq->cq_fid.ops == hard_ops) {

		/* save the CQ before we trash the union */
		ucq = cq->c.hard.cq_cq;

		/* fill in the soft part of union */
		TAILQ_INIT(&cq->c.soft.cq_list);
		cq->c.soft.cq_comps = calloc(cq->cq_attr.size,
					sizeof(struct usdf_cq_soft_entry));
		if (cq->c.soft.cq_comps == NULL) {
			return -FI_ENOMEM;
		}
		cq->c.soft.cq_end = cq->c.soft.cq_comps + cq->cq_attr.size;
		cq->c.soft.cq_head = cq->c.soft.cq_comps;
		cq->c.soft.cq_tail = cq->c.soft.cq_comps;

		/* need to add hard queue to list? */
		if (ucq != NULL) {
			hcq = malloc(sizeof(*hcq));
			if (hcq == NULL) {
				free(cq->c.soft.cq_comps);
				cq->c.hard.cq_cq = ucq;	/* restore */
				return -FI_ENOMEM;
			}

			hcq->cqh_cq = cq;
			hcq->cqh_ucq = ucq;
			hcq->cqh_progress = rtn;

			atomic_initialize(&hcq->cqh_refcnt,
					atomic_get(&cq->cq_refcnt));
			TAILQ_INSERT_HEAD(&cq->c.soft.cq_list, hcq, cqh_link);
		}

                cq->cq_fid.ops = soft_ops;
        }
	return 0;
}

static int
usdf_cq_process_attr(struct fi_cq_attr *attr, struct usdf_domain *udp)
{
	/* no wait object yet */
	if (attr->wait_obj != FI_WAIT_NONE) {
		return -FI_ENOSYS;
	}

	/* bound and default size */
	if (attr->size > udp->dom_fabric->fab_dev_attrs->uda_max_cqe) {
		return -FI_EINVAL;
	}
	if (attr->size == 0) {
		attr->size = udp->dom_fabric->fab_dev_attrs->uda_max_cqe;
	}

	/* default format is FI_CQ_FORMAT_CONTEXT */
	if (attr->format == FI_CQ_FORMAT_UNSPEC) {

		attr->format = FI_CQ_FORMAT_CONTEXT;
	}
	return 0;
}

int
usdf_cq_create_cq(struct usdf_cq *cq)
{
	return usd_create_cq(cq->cq_domain->dom_dev, cq->cq_attr.size, -1,
			&cq->c.hard.cq_cq);
}

int
usdf_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
	    struct fid_cq **cq_o, void *context)
{
	struct usdf_cq *cq;
	struct usdf_domain *udp;
	int ret;

	USDF_TRACE_SYS(CQ, "\n");

	udp = dom_ftou(domain);
	ret = usdf_cq_process_attr(attr, udp);
	if (ret != 0) {
		return ret;
	}

	cq = calloc(1, sizeof(*cq));
	if (cq == NULL) {
		return -FI_ENOMEM;
	}

	cq->cq_domain = udp;
	cq->cq_fid.fid.fclass = FI_CLASS_CQ;
	cq->cq_fid.fid.context = context;
	cq->cq_fid.fid.ops = &usdf_cq_fi_ops;
	atomic_initialize(&cq->cq_refcnt, 0);

	switch (attr->format) {
	case FI_CQ_FORMAT_CONTEXT:
		cq->cq_fid.ops = &usdf_cq_context_ops;
		break;
	case FI_CQ_FORMAT_MSG:
		cq->cq_fid.ops = &usdf_cq_msg_ops;
		break;
	case FI_CQ_FORMAT_DATA:
		cq->cq_fid.ops = &usdf_cq_data_ops;
		break;
	default:
		ret = -FI_ENOSYS;
		goto fail;
	}

	cq->cq_attr = *attr;
	*cq_o = &cq->cq_fid;
	return 0;

fail:
	if (cq != NULL) {
		if (cq->c.hard.cq_cq != NULL) {
			usd_destroy_cq(cq->c.hard.cq_cq);
		}
		free(cq);
	}
	return ret;
}
