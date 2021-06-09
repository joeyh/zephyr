/*
 * Copyright (c) 2020 Demant
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>

#include <bluetooth/hci.h>
#include <sys/byteorder.h>
#include <sys/slist.h>
#include <sys/util.h>

#include "hal/ccm.h"

#include "util/mem.h"
#include "util/memq.h"

#include "pdu.h"
#include "ll.h"
#include "ll_feat.h"
#include "ll_settings.h"

#include "lll.h"
#include "lll_conn.h"

#include "ull_tx_queue.h"
#include "ull_conn_llcp_internal.h"
#include "ull_conn_types.h"
#include "ull_internal.h"
#include "ull_llcp.h"
#include "ull_llcp_features.h"
#include "ull_llcp_internal.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_DEBUG_HCI_DRIVER)
#define LOG_MODULE_NAME bt_ctlr_ull_llcp_conn_upd
#include "common/log.h"
#include <soc.h>
#include "hal/debug.h"

/* Hardcoded instant delta +6 */
#define CONN_UPDATE_INSTANT_DELTA  6U

/* TODO: Known, missing items (missing implementation):
 *	LL/CON/MAS/BV-34-C [Accepting Connection Parameter Request – event masked]
 */

/* LLCP Local Procedure Connection Update FSM states */
enum {
	LP_CU_STATE_IDLE,
	LP_CU_STATE_WAIT_TX_CONN_PARAM_REQ,
	LP_CU_STATE_WAIT_RX_CONN_PARAM_RSP,
	LP_CU_STATE_WAIT_TX_CONN_UPDATE_IND,
	LP_CU_STATE_WAIT_RX_CONN_UPDATE_IND,
	LP_CU_STATE_WAIT_INSTANT,
	LP_CU_STATE_WAIT_NTF,
};

/* LLCP Local Procedure Connection Update FSM events */
enum {
	/* Procedure run */
	LP_CU_EVT_RUN,

	/* Response recieved */
	LP_CU_EVT_CONN_PARAM_RSP,

	/* Indication recieved */
	LP_CU_EVT_CONN_UPDATE_IND,

	/* Reject response recieved */
	LP_CU_EVT_REJECT,

	/* Unknown response recieved */
	LP_CU_EVT_UNKNOWN,
};

/* LLCP Remote Procedure Connection Update FSM states */
enum {
	RP_CU_STATE_IDLE,
	RP_CU_STATE_WAIT_RX_CONN_PARAM_REQ,
	RP_CU_STATE_WAIT_NTF_CONN_PARAM_REQ,
	RP_CU_STATE_WAIT_CONN_PARAM_REQ_REPLY,
	RP_CU_STATE_WAIT_TX_REJECT_EXT_IND,
	RP_CU_STATE_WAIT_TX_CONN_PARAM_RSP,
	RP_CU_STATE_WAIT_TX_CONN_UPDATE_IND,
	RP_CU_STATE_WAIT_RX_CONN_UPDATE_IND,
	RP_CU_STATE_WAIT_INSTANT,
	RP_CU_STATE_WAIT_NTF,
	RP_CU_STATE_WAIT_TX_UNKNOWN_RSP
};

/* LLCP Remote Procedure Connection Update FSM events */
enum {
	/* Procedure run */
	RP_CU_EVT_RUN,

	/* Request recieved */
	RP_CU_EVT_CONN_PARAM_REQ,

	/* Indication recieved */
	RP_CU_EVT_CONN_UPDATE_IND,

	/* CONN_PARAM_REQ reply */
	RP_CU_EVT_CONN_PARAM_REQ_REPLY,

	/* CONN_PARAM_REQ negative reply */
	RP_CU_EVT_CONN_PARAM_REQ_NEG_REPLY,
};

/*
 * LLCP Local Procedure Connection Update FSM
 */

static bool cu_have_params_changed(struct ll_conn *conn, uint16_t interval,
				uint16_t latency, uint16_t timeout)
{
	struct lll_conn *lll = &conn->lll;

	if ((interval != lll->interval) ||
	    (latency != lll->latency) ||
	    (RADIO_CONN_EVENTS(timeout * 10000U,
			       lll->interval * CONN_INT_UNIT_US) !=
	     conn->supervision_reload)) {
		return true;
	}
	return false;
}

static void cu_update_conn_parameters(struct ll_conn *conn, struct proc_ctx *ctx)
{
	ctx->data.cu.params_changed = cu_have_params_changed(conn,
					ctx->data.cu.interval_max,
					ctx->data.cu.latency,
					ctx->data.cu.timeout);

	ull_conn_update_parameters(conn, (ctx->proc == PROC_CONN_UPDATE),
					ctx->data.cu.win_size,
					ctx->data.cu.win_offset_us,
					ctx->data.cu.interval_max,
					ctx->data.cu.latency,
					ctx->data.cu.timeout,
					ctx->data.cu.instant);
}

static bool cu_should_notify_host(struct proc_ctx *ctx)
{
	return (((ctx->proc == PROC_CONN_PARAM_REQ) && (ctx->data.cu.error != 0U)) ||
		 (ctx->data.cu.params_changed != 0U));
}

static void lp_cu_tx(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t opcode)
{
	struct node_tx *tx;
	struct pdu_data *pdu;

	/* Allocate tx node */
	tx = tx_alloc();
	LL_ASSERT(tx);

	pdu = (struct pdu_data *)tx->pdu;

	/* Encode LL Control PDU */
	switch (opcode) {
	case PDU_DATA_LLCTRL_TYPE_CONN_PARAM_REQ:
		pdu_encode_conn_param_req(ctx, pdu);
		break;
	case PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_IND:
		pdu_encode_conn_update_ind(ctx, pdu);
		break;
	default:
		LL_ASSERT(0);
		break;
	}

	ctx->tx_opcode = pdu->llctrl.opcode;

	/* Enqueue LL Control PDU towards LLL */
	tx_enqueue(conn, tx);
}

static void lp_cu_ntf(struct ll_conn *conn, struct proc_ctx *ctx)
{
	struct node_rx_pdu *ntf;
	struct node_rx_cu *pdu;

	/* Allocate ntf node */
	ntf = ntf_alloc();
	LL_ASSERT(ntf);

	ntf->hdr.type = NODE_RX_TYPE_CONN_UPDATE;
	ntf->hdr.handle = conn->lll.handle;
	pdu = (struct node_rx_cu *)ntf->pdu;

	pdu->status = ctx->data.cu.error;

	/* Enqueue notification towards LL */
	ll_rx_put(ntf->hdr.link, ntf);
	ll_rx_sched();
}

static void lp_cu_complete(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	if (!ntf_alloc_is_available()) {
		ctx->state = LP_CU_STATE_WAIT_NTF;
	} else {
		lp_cu_ntf(conn, ctx);
		lr_complete(conn);
		ctx->state = LP_CU_STATE_IDLE;
	}
}

static void lp_cu_send_conn_param_req(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	if (!tx_alloc_is_available() || rr_get_collision(conn)) {
		ctx->state = LP_CU_STATE_WAIT_TX_CONN_PARAM_REQ;
	} else {
		uint16_t event_counter = ull_conn_event_counter(conn);

		rr_set_incompat(conn, INCOMPAT_RESOLVABLE);

		ctx->data.cu.reference_conn_event_count = event_counter;
		ctx->data.cu.preferred_periodicity = 0U;
		ctx->data.cu.offset0 = 0x0000U;
		ctx->data.cu.offset1 = 0xffffU;
		ctx->data.cu.offset2 = 0xffffU;
		ctx->data.cu.offset3 = 0xffffU;
		ctx->data.cu.offset4 = 0xffffU;
		ctx->data.cu.offset5 = 0xffffU;

		lp_cu_tx(conn, ctx, PDU_DATA_LLCTRL_TYPE_CONN_PARAM_REQ);

		switch (conn->lll.role) {
		case BT_HCI_ROLE_MASTER:
			ctx->state = LP_CU_STATE_WAIT_RX_CONN_PARAM_RSP;
			ctx->rx_opcode = PDU_DATA_LLCTRL_TYPE_CONN_PARAM_RSP;
			break;
		case BT_HCI_ROLE_SLAVE:
			ctx->state = LP_CU_STATE_WAIT_RX_CONN_UPDATE_IND;
			ctx->rx_opcode = PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_IND;
			break;
		default:
			/* Unknown role */
			LL_ASSERT(0);
			break;
		}
	}
}

static void lp_cu_send_conn_update_ind(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	if (!tx_alloc_is_available()) {
		ctx->state = LP_CU_STATE_WAIT_TX_CONN_UPDATE_IND;
	} else {
		/* TODO(thoh): Hardcoded instant delta +6 */
		ctx->data.cu.instant = ull_conn_event_counter(conn) + CONN_UPDATE_INSTANT_DELTA;
		lp_cu_tx(conn, ctx, PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_IND);
		ctx->rx_opcode = PDU_DATA_LLCTRL_TYPE_UNUSED;
		ctx->state = LP_CU_STATE_WAIT_INSTANT;
	}
}

static void lp_cu_st_idle(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case LP_CU_EVT_RUN:
		switch (ctx->proc) {
		case PROC_CONN_PARAM_REQ:
			lp_cu_send_conn_param_req(conn, ctx, evt, param);
			break;
		case PROC_CONN_UPDATE:
			lp_cu_send_conn_update_ind(conn, ctx, evt, param);
			break;
		default:
			/* Unknown procedure */
			LL_ASSERT(0);
			break;
		}
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lp_cu_st_wait_tx_conn_param_req(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case LP_CU_EVT_RUN:
		lp_cu_send_conn_param_req(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lp_cu_st_wait_rx_conn_param_rsp(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	struct pdu_data *pdu = (struct pdu_data *) param;

	switch (evt) {
	case LP_CU_EVT_CONN_PARAM_RSP:
		rr_set_incompat(conn, INCOMPAT_RESERVED);
		lp_cu_send_conn_update_ind(conn, ctx, evt, param);
		break;
	case LP_CU_EVT_UNKNOWN:
		rr_set_incompat(conn, INCOMPAT_RESERVED);
		/* Unsupported in peer, so disable locally for this connection */
		feature_unmask_features(conn, LL_FEAT_BIT_CONN_PARAM_REQ);
		lp_cu_send_conn_update_ind(conn, ctx, evt, param);
		break;
	case LP_CU_EVT_REJECT:
		/* TODO(tosk): Select between LL_REJECT_IND and LL_REJECT_EXT_IND */
		if (pdu->llctrl.reject_ext_ind.error_code == BT_HCI_ERR_UNSUPP_REMOTE_FEATURE) {
			/* Remote legacy Host */
			rr_set_incompat(conn, INCOMPAT_RESERVED);
			/* Unsupported in peer, so disable locally for this connection */
			feature_unmask_features(conn, LL_FEAT_BIT_CONN_PARAM_REQ);
			lp_cu_send_conn_update_ind(conn, ctx, evt, param);
		} else {
			rr_set_incompat(conn, INCOMPAT_NO_COLLISION);
			ctx->data.cu.error = pdu->llctrl.reject_ext_ind.error_code;
			lp_cu_complete(conn, ctx, evt, param);
		}
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lp_cu_st_wait_tx_conn_update_ind(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case LP_CU_EVT_RUN:
		lp_cu_send_conn_update_ind(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lp_cu_st_wait_rx_conn_update_ind(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	struct pdu_data *pdu = (struct pdu_data *) param;

	switch (evt) {
	case LP_CU_EVT_CONN_UPDATE_IND:
		pdu_decode_conn_update_ind(ctx, param);
		ctx->state = LP_CU_STATE_WAIT_INSTANT;
		break;
	case LP_CU_EVT_UNKNOWN:
		ctx->data.cu.error = BT_HCI_ERR_UNSUPP_REMOTE_FEATURE;
		lp_cu_complete(conn, ctx, evt, param);
		break;
	case LP_CU_EVT_REJECT:
		ctx->data.cu.error = pdu->llctrl.reject_ext_ind.error_code;
		lp_cu_complete(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lp_cu_check_instant(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	uint16_t event_counter = ull_conn_event_counter(conn);

	if (is_instant_reached_or_passed(ctx->data.cu.instant, event_counter)) {
		bool notify;

		/* Procedure is complete when the instant has passed, and the
		 * new connection event parameters have been applied.
		 */
		cu_update_conn_parameters(conn, ctx);
		notify = cu_should_notify_host(ctx);
		if (notify) {
			rr_set_incompat(conn, INCOMPAT_NO_COLLISION);
			ctx->data.cu.error = BT_HCI_ERR_SUCCESS;
			lp_cu_complete(conn, ctx, evt, param);
		} else {
			lr_complete(conn);
			ctx->state = LP_CU_STATE_IDLE;
		}
	}
}

static void lp_cu_st_wait_instant(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	/* TODO */
	switch (evt) {
	case LP_CU_EVT_RUN:
		lp_cu_check_instant(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lp_cu_st_wait_ntf(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case LP_CU_EVT_RUN:
		lp_cu_complete(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lp_cu_execute_fsm(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (ctx->state) {
	case LP_CU_STATE_IDLE:
		lp_cu_st_idle(conn, ctx, evt, param);
		break;
	case LP_CU_STATE_WAIT_TX_CONN_PARAM_REQ:
		lp_cu_st_wait_tx_conn_param_req(conn, ctx, evt, param);
		break;
	case LP_CU_STATE_WAIT_RX_CONN_PARAM_RSP:
		lp_cu_st_wait_rx_conn_param_rsp(conn, ctx, evt, param);
		break;
	case LP_CU_STATE_WAIT_TX_CONN_UPDATE_IND:
		lp_cu_st_wait_tx_conn_update_ind(conn, ctx, evt, param);
		break;
	case LP_CU_STATE_WAIT_RX_CONN_UPDATE_IND:
		lp_cu_st_wait_rx_conn_update_ind(conn, ctx, evt, param);
		break;
	case LP_CU_STATE_WAIT_INSTANT:
		lp_cu_st_wait_instant(conn, ctx, evt, param);
		break;
	case LP_CU_STATE_WAIT_NTF:
		lp_cu_st_wait_ntf(conn, ctx, evt, param);
		break;
	default:
		/* Unknown state */
		LL_ASSERT(0);
		break;
	}
}

void ull_cp_priv_lp_cu_rx(struct ll_conn *conn, struct proc_ctx *ctx, struct node_rx_pdu *rx)
{
	struct pdu_data *pdu = (struct pdu_data *) rx->pdu;

	switch (pdu->llctrl.opcode) {
	case PDU_DATA_LLCTRL_TYPE_CONN_PARAM_RSP:
		lp_cu_execute_fsm(conn, ctx, LP_CU_EVT_CONN_PARAM_RSP, pdu);
		break;
	case PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_IND:
		lp_cu_execute_fsm(conn, ctx, LP_CU_EVT_CONN_UPDATE_IND, pdu);
		break;
	case PDU_DATA_LLCTRL_TYPE_UNKNOWN_RSP:
		lp_cu_execute_fsm(conn, ctx, LP_CU_EVT_UNKNOWN, pdu);
		break;
	case PDU_DATA_LLCTRL_TYPE_REJECT_EXT_IND:
		lp_cu_execute_fsm(conn, ctx, LP_CU_EVT_REJECT, pdu);
		break;
	default:
		/* Unknown opcode */
		LL_ASSERT(0);
		break;
	}
}

void ull_cp_priv_lp_cu_init_proc(struct proc_ctx *ctx)
{
	ctx->state = LP_CU_STATE_IDLE;
}

void ull_cp_priv_lp_cu_run(struct ll_conn *conn, struct proc_ctx *ctx, void *param)
{
	lp_cu_execute_fsm(conn, ctx, LP_CU_EVT_RUN, param);
}

/*
 * LLCP Remote Procedure Connection Update FSM
 */

static void rp_cu_tx(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t opcode)
{
	struct node_tx *tx;
	struct pdu_data *pdu;

	/* Allocate tx node */
	tx = tx_alloc();
	LL_ASSERT(tx);

	pdu = (struct pdu_data *)tx->pdu;

	/* Encode LL Control PDU */
	switch (opcode) {
	case PDU_DATA_LLCTRL_TYPE_CONN_PARAM_RSP:
		pdu_encode_conn_param_rsp(ctx, pdu);
		break;
	case PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_IND:
		pdu_encode_conn_update_ind(ctx, pdu);
		break;
	case PDU_DATA_LLCTRL_TYPE_REJECT_EXT_IND:
		/* TODO(thoh): Select between LL_REJECT_IND and LL_REJECT_EXT_IND */
		pdu_encode_reject_ext_ind(pdu, PDU_DATA_LLCTRL_TYPE_CONN_PARAM_REQ, ctx->data.cu.error);
		break;
	case PDU_DATA_LLCTRL_TYPE_UNKNOWN_RSP:
		pdu_encode_unknown_rsp(ctx, pdu);
		break;
	default:
		LL_ASSERT(0);
		break;
	}

	ctx->tx_opcode = pdu->llctrl.opcode;

	/* Enqueue LL Control PDU towards LLL */
	tx_enqueue(conn, tx);
}

static void rp_cu_ntf(struct ll_conn *conn, struct proc_ctx *ctx)
{
	struct node_rx_pdu *ntf;
	struct node_rx_cu *pdu;

	/* Allocate ntf node */
	ntf = ntf_alloc();
	LL_ASSERT(ntf);

	ntf->hdr.type = NODE_RX_TYPE_CONN_UPDATE;
	ntf->hdr.handle = conn->lll.handle;
	pdu = (struct node_rx_cu *)ntf->pdu;

	pdu->status = ctx->data.cu.error;

	/* Enqueue notification towards LL */
	ll_rx_put(ntf->hdr.link, ntf);
	ll_rx_sched();
}

static void rp_cu_conn_param_req_ntf(struct ll_conn *conn, struct proc_ctx *ctx)
{
	struct node_rx_pdu *ntf;
	struct pdu_data *pdu;

	/* Allocate ntf node */
	ntf = ntf_alloc();
	LL_ASSERT(ntf);

	ntf->hdr.type = NODE_RX_TYPE_DC_PDU;
	ntf->hdr.handle = conn->lll.handle;
	pdu = (struct pdu_data *) ntf->pdu;

	pdu_encode_conn_param_req(ctx, pdu);

	/* Enqueue notification towards LL */
	ll_rx_put(ntf->hdr.link, ntf);
	ll_rx_sched();
}

static void rp_cu_complete(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	if (!ntf_alloc_is_available()) {
		ctx->state = RP_CU_STATE_WAIT_NTF;
	} else {
		rp_cu_ntf(conn, ctx);
		rr_complete(conn);
		ctx->state = RP_CU_STATE_IDLE;
	}
}

static void rp_cu_send_conn_update_ind(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	if (!tx_alloc_is_available()) {
		ctx->state = RP_CU_STATE_WAIT_TX_CONN_UPDATE_IND;
	} else {
		/* TODO(thoh): Hardcoded instant delta +6 */
		ctx->data.cu.instant = ull_conn_event_counter(conn) + CONN_UPDATE_INSTANT_DELTA;
		rp_cu_tx(conn, ctx, PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_IND);
		ctx->rx_opcode = PDU_DATA_LLCTRL_TYPE_UNUSED;
		ctx->state = RP_CU_STATE_WAIT_INSTANT;
	}
}

static void rp_cu_send_reject_ext_ind(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	if (!tx_alloc_is_available()) {
		ctx->state = RP_CU_STATE_WAIT_TX_REJECT_EXT_IND;
	} else {
		rp_cu_tx(conn, ctx, PDU_DATA_LLCTRL_TYPE_REJECT_EXT_IND);
		rr_complete(conn);
		ctx->state = RP_CU_STATE_IDLE;
	}
}

static void rp_cu_send_conn_param_rsp(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	if (!tx_alloc_is_available()) {
		ctx->state = RP_CU_STATE_WAIT_TX_CONN_PARAM_RSP;
	} else {
		rp_cu_tx(conn, ctx, PDU_DATA_LLCTRL_TYPE_CONN_PARAM_RSP);
		ctx->rx_opcode = PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_IND;
		ctx->state = RP_CU_STATE_WAIT_RX_CONN_UPDATE_IND;
	}
}

static void rp_cu_send_conn_param_req_ntf(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	if (!ntf_alloc_is_available()) {
		ctx->state = RP_CU_STATE_WAIT_NTF_CONN_PARAM_REQ;
	} else {
		rp_cu_conn_param_req_ntf(conn, ctx);
		ctx->state = RP_CU_STATE_WAIT_CONN_PARAM_REQ_REPLY;
	}
}

static void rp_cu_send_unknown_rsp(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	if (!tx_alloc_is_available()) {
		ctx->state = RP_CU_STATE_WAIT_TX_UNKNOWN_RSP;
	} else {
		rp_cu_tx(conn, ctx, PDU_DATA_LLCTRL_TYPE_UNKNOWN_RSP);
		rr_complete(conn);
		ctx->state = RP_CU_STATE_IDLE;
	}
}

static void rp_cu_st_idle(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case RP_CU_EVT_RUN:
		switch (ctx->proc) {
		case PROC_CONN_PARAM_REQ:
			ctx->state = RP_CU_STATE_WAIT_RX_CONN_PARAM_REQ;
			break;
		case PROC_CONN_UPDATE:
			ctx->state = RP_CU_STATE_WAIT_RX_CONN_UPDATE_IND;
			break;
		default:
			/* Unknown proceduce */
			LL_ASSERT(0);
			break;
		}
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_cu_st_wait_rx_conn_param_req(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case RP_CU_EVT_CONN_PARAM_REQ:
		pdu_decode_conn_param_req(ctx, param);

		bool params_changed = cu_have_params_changed(conn,
						ctx->data.cu.interval_max,
						ctx->data.cu.latency,
						ctx->data.cu.timeout);

		/* notify Host if conn parameters changed, else respond */
		if (params_changed) {
			rp_cu_send_conn_param_req_ntf(conn, ctx, evt, param);
		} else {
			ctx->state = RP_CU_STATE_WAIT_CONN_PARAM_REQ_REPLY;
		}
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_cu_state_wait_ntf_conn_param_req(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case RP_CU_EVT_RUN:
		rp_cu_send_conn_param_req_ntf(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_cu_state_wait_conn_param_req_reply(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case RP_CU_EVT_CONN_PARAM_REQ_REPLY:
		if (conn->lll.role == BT_HCI_ROLE_MASTER) {
			rp_cu_send_conn_update_ind(conn, ctx, evt, param);
		} else if (conn->lll.role == BT_HCI_ROLE_SLAVE) {
			rp_cu_send_conn_param_rsp(conn, ctx, evt, param);
		} else {
			/* Unknown role */
			LL_ASSERT(0);
		}
		break;
	case RP_CU_EVT_CONN_PARAM_REQ_NEG_REPLY:
		rp_cu_send_reject_ext_ind(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_cu_state_wait_tx_reject_ext_ind(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case RP_CU_EVT_RUN:
		rp_cu_send_reject_ext_ind(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_cu_st_wait_tx_conn_param_rsp(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case RP_CU_EVT_RUN:
		rp_cu_send_conn_param_rsp(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_cu_st_wait_tx_conn_update_ind(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case RP_CU_EVT_RUN:
		rp_cu_send_conn_update_ind(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_cu_st_wait_rx_conn_update_ind(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case RP_CU_EVT_CONN_UPDATE_IND:
		switch (conn->lll.role) {
		case BT_HCI_ROLE_MASTER:
			ctx->unknown_response.type = PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_IND;
			rp_cu_send_unknown_rsp(conn, ctx, evt, param);
			break;
		case BT_HCI_ROLE_SLAVE:
			pdu_decode_conn_update_ind(ctx, param);
			/* TODO(tosk): skip/terminate if instant passed? */
#if defined(CONFIG_BT_CTLR_CONN_PARAM_REQ)
			/* conn param req procedure, if any, is complete */
			 ull_conn_prt_clear(conn);
#endif /* CONFIG_BT_CTLR_CONN_PARAM_REQ */
			ctx->state = RP_CU_STATE_WAIT_INSTANT;
			break;
		default:
			/* Unknown role */
			LL_ASSERT(0);
		}
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_cu_check_instant(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	uint16_t event_counter = ull_conn_event_counter(conn);

	if (is_instant_reached_or_passed(ctx->data.cu.instant, event_counter)) {
		bool notify;

		/* Procedure is complete when the instant has passed, and the
		 * new connection event parameters have been applied.
		 */
		cu_update_conn_parameters(conn, ctx);
		notify = cu_should_notify_host(ctx);
		if (notify) {
			ctx->data.cu.error = BT_HCI_ERR_SUCCESS;
			rp_cu_complete(conn, ctx, evt, param);
		} else {
			rr_complete(conn);
			ctx->state = RP_CU_STATE_IDLE;
		}
	}
}

static void rp_cu_st_wait_instant(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case RP_CU_EVT_RUN:
		rp_cu_check_instant(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_cu_st_wait_ntf(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (evt) {
	case RP_CU_EVT_RUN:
		rp_cu_complete(conn, ctx, evt, param);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_cu_execute_fsm(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t evt, void *param)
{
	switch (ctx->state) {
	case RP_CU_STATE_IDLE:
		rp_cu_st_idle(conn, ctx, evt, param);
		break;
	case RP_CU_STATE_WAIT_RX_CONN_PARAM_REQ:
		rp_cu_st_wait_rx_conn_param_req(conn, ctx, evt, param);
		break;
	case RP_CU_STATE_WAIT_NTF_CONN_PARAM_REQ:
		rp_cu_state_wait_ntf_conn_param_req(conn, ctx, evt, param);
		break;
	case RP_CU_STATE_WAIT_CONN_PARAM_REQ_REPLY:
		rp_cu_state_wait_conn_param_req_reply(conn, ctx, evt, param);
		break;
	case RP_CU_STATE_WAIT_TX_REJECT_EXT_IND:
		rp_cu_state_wait_tx_reject_ext_ind(conn, ctx, evt, param);
		break;
	case RP_CU_STATE_WAIT_TX_CONN_PARAM_RSP:
		rp_cu_st_wait_tx_conn_param_rsp(conn, ctx, evt, param);
		break;
	case RP_CU_STATE_WAIT_TX_CONN_UPDATE_IND:
		rp_cu_st_wait_tx_conn_update_ind(conn, ctx, evt, param);
		break;
	case RP_CU_STATE_WAIT_RX_CONN_UPDATE_IND:
		rp_cu_st_wait_rx_conn_update_ind(conn, ctx, evt, param);
		break;
	case RP_CU_STATE_WAIT_INSTANT:
		rp_cu_st_wait_instant(conn, ctx, evt, param);
		break;
	case RP_CU_STATE_WAIT_NTF:
		rp_cu_st_wait_ntf(conn, ctx, evt, param);
		break;
	default:
		/* Unknown state */
		LL_ASSERT(0);
		break;
	}
}

void ull_cp_priv_rp_cu_rx(struct ll_conn *conn, struct proc_ctx *ctx, struct node_rx_pdu *rx)
{
	struct pdu_data *pdu = (struct pdu_data *) rx->pdu;

	switch (pdu->llctrl.opcode) {
	case PDU_DATA_LLCTRL_TYPE_CONN_PARAM_REQ:
		rp_cu_execute_fsm(conn, ctx, RP_CU_EVT_CONN_PARAM_REQ, pdu);
		break;
	case PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_IND:
		rp_cu_execute_fsm(conn, ctx, RP_CU_EVT_CONN_UPDATE_IND, pdu);
		break;
	default:
		/* Unknown opcode */
		LL_ASSERT(0);
		break;
	}
}

void ull_cp_priv_rp_cu_init_proc(struct proc_ctx *ctx)
{
	ctx->state = RP_CU_STATE_IDLE;
}

void ull_cp_priv_rp_cu_run(struct ll_conn *conn, struct proc_ctx *ctx, void *param)
{
	rp_cu_execute_fsm(conn, ctx, RP_CU_EVT_RUN, param);
}

void ull_cp_priv_rp_conn_param_req_reply(struct ll_conn *conn, struct proc_ctx *ctx)
{
	rp_cu_execute_fsm(conn, ctx, RP_CU_EVT_CONN_PARAM_REQ_REPLY, NULL);
}

void ull_cp_priv_rp_conn_param_req_neg_reply(struct ll_conn *conn, struct proc_ctx *ctx)
{
	rp_cu_execute_fsm(conn, ctx, RP_CU_EVT_CONN_PARAM_REQ_NEG_REPLY, NULL);
}