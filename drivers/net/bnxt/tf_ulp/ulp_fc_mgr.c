/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2014-2020 Broadcom
 * All rights reserved.
 */

#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_alarm.h>
#include "bnxt.h"
#include "bnxt_ulp.h"
#include "bnxt_tf_common.h"
#include "ulp_fc_mgr.h"
#include "ulp_template_db_enum.h"
#include "ulp_template_struct.h"
#include "tf_tbl.h"

static int
ulp_fc_mgr_shadow_mem_alloc(struct hw_fc_mem_info *parms, int size)
{
	/* Allocate memory*/
	if (parms == NULL)
		return -EINVAL;

	parms->mem_va = rte_zmalloc("ulp_fc_info",
				    RTE_CACHE_LINE_ROUNDUP(size),
				    4096);
	if (parms->mem_va == NULL) {
		BNXT_TF_DBG(ERR, "Allocate failed mem_va\n");
		return -ENOMEM;
	}

	rte_mem_lock_page(parms->mem_va);

	parms->mem_pa = (void *)(uintptr_t)rte_mem_virt2phy(parms->mem_va);
	if (parms->mem_pa == (void *)(uintptr_t)RTE_BAD_IOVA) {
		BNXT_TF_DBG(ERR, "Allocate failed mem_pa\n");
		return -ENOMEM;
	}

	return 0;
}

static void
ulp_fc_mgr_shadow_mem_free(struct hw_fc_mem_info *parms)
{
	rte_free(parms->mem_va);
}

/*
 * Allocate and Initialize all Flow Counter Manager resources for this ulp
 * context.
 *
 * ctxt [in] The ulp context for the Flow Counter manager.
 *
 */
int32_t
ulp_fc_mgr_init(struct bnxt_ulp_context *ctxt)
{
	struct bnxt_ulp_device_params *dparms;
	uint32_t dev_id, sw_acc_cntr_tbl_sz, hw_fc_mem_info_sz;
	struct bnxt_ulp_fc_info *ulp_fc_info;
	int i, rc;

	if (!ctxt) {
		BNXT_TF_DBG(DEBUG, "Invalid ULP CTXT\n");
		return -EINVAL;
	}

	if (bnxt_ulp_cntxt_dev_id_get(ctxt, &dev_id)) {
		BNXT_TF_DBG(DEBUG, "Failed to get device id\n");
		return -EINVAL;
	}

	dparms = bnxt_ulp_device_params_get(dev_id);
	if (!dparms) {
		BNXT_TF_DBG(DEBUG, "Failed to device parms\n");
		return -EINVAL;
	}

	ulp_fc_info = rte_zmalloc("ulp_fc_info", sizeof(*ulp_fc_info), 0);
	if (!ulp_fc_info)
		goto error;

	rc = pthread_mutex_init(&ulp_fc_info->fc_lock, NULL);
	if (rc) {
		PMD_DRV_LOG(ERR, "Failed to initialize fc mutex\n");
		goto error;
	}

	/* Add the FC info tbl to the ulp context. */
	bnxt_ulp_cntxt_ptr2_fc_info_set(ctxt, ulp_fc_info);

	sw_acc_cntr_tbl_sz = sizeof(struct sw_acc_counter) *
				dparms->flow_count_db_entries;

	for (i = 0; i < TF_DIR_MAX; i++) {
		ulp_fc_info->sw_acc_tbl[i] = rte_zmalloc("ulp_sw_acc_cntr_tbl",
							 sw_acc_cntr_tbl_sz, 0);
		if (!ulp_fc_info->sw_acc_tbl[i])
			goto error;
	}

	hw_fc_mem_info_sz = sizeof(uint64_t) * dparms->flow_count_db_entries;

	for (i = 0; i < TF_DIR_MAX; i++) {
		rc = ulp_fc_mgr_shadow_mem_alloc(&ulp_fc_info->shadow_hw_tbl[i],
						 hw_fc_mem_info_sz);
		if (rc)
			goto error;
	}

	return 0;

error:
	ulp_fc_mgr_deinit(ctxt);
	BNXT_TF_DBG(DEBUG,
		    "Failed to allocate memory for fc mgr\n");

	return -ENOMEM;
}

/*
 * Release all resources in the Flow Counter Manager for this ulp context
 *
 * ctxt [in] The ulp context for the Flow Counter manager
 *
 */
int32_t
ulp_fc_mgr_deinit(struct bnxt_ulp_context *ctxt)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;
	int i;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);

	if (!ulp_fc_info)
		return -EINVAL;

	ulp_fc_mgr_thread_cancel(ctxt);

	pthread_mutex_destroy(&ulp_fc_info->fc_lock);

	for (i = 0; i < TF_DIR_MAX; i++)
		rte_free(ulp_fc_info->sw_acc_tbl[i]);

	for (i = 0; i < TF_DIR_MAX; i++)
		ulp_fc_mgr_shadow_mem_free(&ulp_fc_info->shadow_hw_tbl[i]);


	rte_free(ulp_fc_info);

	/* Safe to ignore on deinit */
	(void)bnxt_ulp_cntxt_ptr2_fc_info_set(ctxt, NULL);

	return 0;
}

/*
 * Check if the alarm thread that walks through the flows is started
 *
 * ctxt [in] The ulp context for the flow counter manager
 *
 */
bool ulp_fc_mgr_thread_isstarted(struct bnxt_ulp_context *ctxt)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);

	return !!(ulp_fc_info->flags & ULP_FLAG_FC_THREAD);
}

/*
 * Setup the Flow counter timer thread that will fetch/accumulate raw counter
 * data from the chip's internal flow counters
 *
 * ctxt [in] The ulp context for the flow counter manager
 *
 */
int32_t
ulp_fc_mgr_thread_start(struct bnxt_ulp_context *ctxt)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);

	if (!(ulp_fc_info->flags & ULP_FLAG_FC_THREAD)) {
		rte_eal_alarm_set(US_PER_S * ULP_FC_TIMER,
				  ulp_fc_mgr_alarm_cb,
				  (void *)ctxt);
		ulp_fc_info->flags |= ULP_FLAG_FC_THREAD;
	}

	return 0;
}

/*
 * Cancel the alarm handler
 *
 * ctxt [in] The ulp context for the flow counter manager
 *
 */
void ulp_fc_mgr_thread_cancel(struct bnxt_ulp_context *ctxt)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);
	if (!ulp_fc_info)
		return;

	ulp_fc_info->flags &= ~ULP_FLAG_FC_THREAD;
	rte_eal_alarm_cancel(ulp_fc_mgr_alarm_cb, (void *)ctxt);
}

/*
 * DMA-in the raw counter data from the HW and accumulate in the
 * local accumulator table using the TF-Core API
 *
 * tfp [in] The TF-Core context
 *
 * fc_info [in] The ULP Flow counter info ptr
 *
 * dir [in] The direction of the flow
 *
 * num_counters [in] The number of counters
 *
 */
static int32_t ulp_bulk_get_flow_stats(struct tf *tfp,
				       struct bnxt_ulp_fc_info *fc_info,
				       enum tf_dir dir, uint32_t num_counters)
{
	int rc = 0;
	struct tf_tbl_get_bulk_parms parms = { 0 };
	enum tf_tbl_type stype = TF_TBL_TYPE_ACT_STATS_64;  /* TBD: Template? */
	struct sw_acc_counter *sw_acc_tbl_entry = NULL;
	uint64_t *stats = NULL;
	uint16_t i = 0;

	parms.dir = dir;
	parms.type = stype;
	parms.starting_idx = fc_info->shadow_hw_tbl[dir].start_idx;
	parms.num_entries = num_counters;
	/*
	 * TODO:
	 * Size of an entry needs to obtained from template
	 */
	parms.entry_sz_in_bytes = sizeof(uint64_t);
	stats = (uint64_t *)fc_info->shadow_hw_tbl[dir].mem_va;
	parms.physical_mem_addr = (uintptr_t)fc_info->shadow_hw_tbl[dir].mem_pa;

	if (stats == NULL) {
		PMD_DRV_LOG(ERR,
			    "BULK: Memory not initialized id:0x%x dir:%d\n",
			    parms.starting_idx, dir);
		return -EINVAL;
	}

	rc = tf_tbl_bulk_get(tfp, &parms);
	if (rc) {
		PMD_DRV_LOG(ERR,
			    "BULK: Get failed for id:0x%x rc:%d\n",
			    parms.starting_idx, rc);
		return rc;
	}

	for (i = 0; i < num_counters; i++) {
		/* TBD - Get PKT/BYTE COUNT SHIFT/MASK from Template */
		sw_acc_tbl_entry = &fc_info->sw_acc_tbl[dir][i];
		if (!sw_acc_tbl_entry->valid)
			continue;
		sw_acc_tbl_entry->pkt_count += FLOW_CNTR_PKTS(stats[i]);
		sw_acc_tbl_entry->byte_count += FLOW_CNTR_BYTES(stats[i]);
	}

	return rc;
}
/*
 * Alarm handler that will issue the TF-Core API to fetch
 * data from the chip's internal flow counters
 *
 * ctxt [in] The ulp context for the flow counter manager
 *
 */
void
ulp_fc_mgr_alarm_cb(void *arg)
{
	int rc = 0, i;
	struct bnxt_ulp_context *ctxt = arg;
	struct bnxt_ulp_fc_info *ulp_fc_info;
	struct bnxt_ulp_device_params *dparms;
	struct tf *tfp;
	uint32_t dev_id;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);
	if (!ulp_fc_info)
		return;

	if (bnxt_ulp_cntxt_dev_id_get(ctxt, &dev_id)) {
		BNXT_TF_DBG(DEBUG, "Failed to get device id\n");
		return;
	}

	dparms = bnxt_ulp_device_params_get(dev_id);
	if (!dparms) {
		BNXT_TF_DBG(DEBUG, "Failed to device parms\n");
		return;
	}

	tfp = bnxt_ulp_cntxt_tfp_get(ctxt);
	if (!tfp) {
		BNXT_TF_DBG(ERR, "Failed to get the truflow pointer\n");
		return;
	}

	/*
	 * Take the fc_lock to ensure no flow is destroyed
	 * during the bulk get
	 */
	if (pthread_mutex_trylock(&ulp_fc_info->fc_lock))
		goto out;

	if (!ulp_fc_info->num_entries) {
		pthread_mutex_unlock(&ulp_fc_info->fc_lock);
		ulp_fc_mgr_thread_cancel(ctxt);
		return;
	}

	for (i = 0; i < TF_DIR_MAX; i++) {
		rc = ulp_bulk_get_flow_stats(tfp, ulp_fc_info, i,
					     dparms->flow_count_db_entries);
		if (rc)
			break;
	}

	pthread_mutex_unlock(&ulp_fc_info->fc_lock);

	/*
	 * If cmd fails once, no need of
	 * invoking again every second
	 */

	if (rc) {
		ulp_fc_mgr_thread_cancel(ctxt);
		return;
	}
out:
	rte_eal_alarm_set(US_PER_S * ULP_FC_TIMER,
			  ulp_fc_mgr_alarm_cb,
			  (void *)ctxt);
}

/*
 * Set the starting index that indicates the first HW flow
 * counter ID
 *
 * ctxt [in] The ulp context for the flow counter manager
 *
 * dir [in] The direction of the flow
 *
 * start_idx [in] The HW flow counter ID
 *
 */
bool ulp_fc_mgr_start_idx_isset(struct bnxt_ulp_context *ctxt, enum tf_dir dir)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);

	/* Assuming start_idx of 0 is invalid */
	return (ulp_fc_info->shadow_hw_tbl[dir].start_idx != 0);
}

/*
 * Set the starting index that indicates the first HW flow
 * counter ID
 *
 * ctxt [in] The ulp context for the flow counter manager
 *
 * dir [in] The direction of the flow
 *
 * start_idx [in] The HW flow counter ID
 *
 */
int32_t ulp_fc_mgr_start_idx_set(struct bnxt_ulp_context *ctxt, enum tf_dir dir,
				 uint32_t start_idx)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);

	if (!ulp_fc_info)
		return -EIO;

	/* Assuming that 0 is an invalid counter ID ? */
	if (ulp_fc_info->shadow_hw_tbl[dir].start_idx == 0)
		ulp_fc_info->shadow_hw_tbl[dir].start_idx = start_idx;

	return 0;
}

/*
 * Set the corresponding SW accumulator table entry based on
 * the difference between this counter ID and the starting
 * counter ID. Also, keep track of num of active counter enabled
 * flows.
 *
 * ctxt [in] The ulp context for the flow counter manager
 *
 * dir [in] The direction of the flow
 *
 * hw_cntr_id [in] The HW flow counter ID
 *
 */
int32_t ulp_fc_mgr_cntr_set(struct bnxt_ulp_context *ctxt, enum tf_dir dir,
			    uint32_t hw_cntr_id)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;
	uint32_t sw_cntr_idx;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);
	if (!ulp_fc_info)
		return -EIO;

	pthread_mutex_lock(&ulp_fc_info->fc_lock);
	sw_cntr_idx = hw_cntr_id - ulp_fc_info->shadow_hw_tbl[dir].start_idx;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].valid = true;
	ulp_fc_info->num_entries++;
	pthread_mutex_unlock(&ulp_fc_info->fc_lock);

	return 0;
}

/*
 * Reset the corresponding SW accumulator table entry based on
 * the difference between this counter ID and the starting
 * counter ID.
 *
 * ctxt [in] The ulp context for the flow counter manager
 *
 * dir [in] The direction of the flow
 *
 * hw_cntr_id [in] The HW flow counter ID
 *
 */
int32_t ulp_fc_mgr_cntr_reset(struct bnxt_ulp_context *ctxt, enum tf_dir dir,
			      uint32_t hw_cntr_id)
{
	struct bnxt_ulp_fc_info *ulp_fc_info;
	uint32_t sw_cntr_idx;

	ulp_fc_info = bnxt_ulp_cntxt_ptr2_fc_info_get(ctxt);
	if (!ulp_fc_info)
		return -EIO;

	pthread_mutex_lock(&ulp_fc_info->fc_lock);
	sw_cntr_idx = hw_cntr_id - ulp_fc_info->shadow_hw_tbl[dir].start_idx;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].valid = false;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].pkt_count = 0;
	ulp_fc_info->sw_acc_tbl[dir][sw_cntr_idx].byte_count = 0;
	ulp_fc_info->num_entries--;
	pthread_mutex_unlock(&ulp_fc_info->fc_lock);

	return 0;
}
