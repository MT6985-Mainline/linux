// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 MediaTek Inc. */
#include <linux/arm-smccc.h>
#include <linux/soc/mediatek/mtk-cmdq-ext.h>
#include "cmdq_sec_mtee.h"

static bool cmdq_mtee;

static KREE_SHAREDMEM_PARAM *cmdq_mtee_wsm_param(struct cmdq_sec_mtee_context *tee,
					       unsigned int index)
{
	KREE_SHAREDMEM_PARAM *params[] = {
		&tee->wsm_param, &tee->wsm_ex_param, &tee->wsm_ex2_param,
	};
	return params[index];
}

static KREE_SHAREDMEM_HANDLE *cmdq_mtee_wsm_handle(struct cmdq_sec_mtee_context *tee,
						 unsigned int index)
{
	KREE_SHAREDMEM_HANDLE *handles[] = {
		&tee->wsm_handle, &tee->wsm_ex_handle, &tee->wsm_ex2_handle,
	};
	return handles[index];
}

void cmdq_sec_mtee_setup_context(struct cmdq_sec_mtee_context *tee)
{
	struct arm_smccc_res res;

	memset(tee, 0, sizeof(*tee));
	strscpy(tee->ta_uuid, "com.mediatek.geniezone.cmdq", sizeof(tee->ta_uuid));
	strscpy(tee->wsm_uuid, "com.mediatek.geniezone.srv.mem", sizeof(tee->wsm_uuid));
	arm_smccc_smc(0xBC00000B, 1, 0, 0, 0, 0, 0, 0, &res);
	cmdq_mtee = res.a0 == 1;
	cmdq_msg("%s a0:%#lx cmdq_mtee:%d", __func__, res.a0, cmdq_mtee);
}

s32 cmdq_sec_mtee_allocate_shared_memory(struct cmdq_sec_mtee_context *tee,
	const dma_addr_t MVABase, const u32 size)
{
	s32 status;

	if (!cmdq_mtee)
		return -EOPNOTSUPP;
	if (!tee->wsm_session_open || !size)
		return -EINVAL;
	if (tee->cookie_registered) {
		if (tee->mem_param.buffer == (void *)(unsigned long)MVABase &&
		    tee->mem_param.size == size)
			return 0;
		status = KREE_UnregisterSharedmem(tee->wsm_pHandle, tee->mem_handle);
		if (status != TZ_RESULT_SUCCESS)
			return status;
		tee->cookie_registered = false;
	}
	memset(&tee->mem_param, 0, sizeof(tee->mem_param));
	tee->mem_param.size = size;
	tee->mem_param.buffer = (void *)(unsigned long)MVABase;
	status = KREE_RegisterSharedmem(tee->wsm_pHandle, &tee->mem_handle,
				       &tee->mem_param);
	if (status == TZ_RESULT_SUCCESS)
		tee->cookie_registered = true;
	else
		cmdq_err("MTEE cookie register session:%#x address:%pad size:%u error:%d",
			 tee->wsm_pHandle, &MVABase, size, status);
	return status;
}

s32 cmdq_sec_mtee_free_wsm(struct cmdq_sec_mtee_context *tee, void **wsm_buffer)
{
	s32 status, first_error = 0;
	unsigned int index;

	if (!wsm_buffer)
		return -EINVAL;
	for (index = 0; index < ARRAY_SIZE(tee->wsm_buffers); index++) {
		if (tee->wsm_registered[index]) {
			status = KREE_UnregisterSharedmem(tee->wsm_pHandle,
						 *cmdq_mtee_wsm_handle(tee, index));
			if (status != TZ_RESULT_SUCCESS) {
				if (!first_error)
					first_error = status;
				cmdq_err("MTEE WSM %u unregister failed:%d", index, status);
				continue;
			}
			tee->wsm_registered[index] = false;
		}
		kfree(tee->wsm_buffers[index]);
		tee->wsm_buffers[index] = NULL;
		memset(cmdq_mtee_wsm_param(tee, index), 0,
		       sizeof(KREE_SHAREDMEM_PARAM));
	}
	*wsm_buffer = tee->wsm_buffers[0];
	return first_error;
}

s32 cmdq_sec_mtee_allocate_wsm(struct cmdq_sec_mtee_context *tee,
	void **wsm_buffer, u32 size, void **wsm_buf_ex, u32 size_ex,
	void **wsm_buf_ex2, u32 size_ex2)
{
	const u32 sizes[] = { size, size_ex, size_ex2 };
	void **outputs[] = { wsm_buffer, wsm_buf_ex, wsm_buf_ex2 };
	s32 status, cleanup;
	unsigned int index;

	if (!cmdq_mtee)
		return -EOPNOTSUPP;
	if (!tee->wsm_session_open || !wsm_buffer || !wsm_buf_ex || !wsm_buf_ex2)
		return -EINVAL;
	for (index = 0; index < ARRAY_SIZE(outputs); index++)
		*outputs[index] = NULL;
	for (index = 0; index < ARRAY_SIZE(sizes); index++) {
		KREE_SHAREDMEM_PARAM *param = cmdq_mtee_wsm_param(tee, index);

		if (!sizes[index]) {
			status = -EINVAL;
			goto unwind;
		}
		if (!tee->wsm_buffers[index]) {
			tee->wsm_buffers[index] = kzalloc(sizes[index], GFP_KERNEL);
			if (!tee->wsm_buffers[index]) {
				status = -ENOMEM;
				goto unwind;
			}
		}
		if (tee->wsm_registered[index])
			continue;
		memset(param, 0, sizeof(*param));
		param->size = sizes[index];
		param->buffer = (void *)(unsigned long)virt_to_phys(tee->wsm_buffers[index]);
		status = KREE_RegisterSharedmem(tee->wsm_pHandle,
					       cmdq_mtee_wsm_handle(tee, index), param);
		if (status != TZ_RESULT_SUCCESS) {
			cmdq_err("MTEE WSM %u register failed:%d", index, status);
			goto unwind;
		}
		tee->wsm_registered[index] = true;
	}
	for (index = 0; index < ARRAY_SIZE(outputs); index++)
		*outputs[index] = tee->wsm_buffers[index];
	return 0;

unwind:
	cleanup = cmdq_sec_mtee_free_wsm(tee, wsm_buffer);
	if (cleanup)
		cmdq_err("MTEE WSM unwind failed:%d; retaining registered buffers", cleanup);
	for (index = 0; index < ARRAY_SIZE(outputs); index++)
		*outputs[index] = NULL;
	return status;
}

s32 cmdq_sec_mtee_open_session(struct cmdq_sec_mtee_context *tee, void *wsm_buffer)
{
	s32 status, cleanup;

	if (!cmdq_mtee)
		return -EOPNOTSUPP;
	if (!tee->session_open) {
		status = KREE_CreateSession(tee->ta_uuid, &tee->pHandle);
		if (status != TZ_RESULT_SUCCESS)
			return status;
		tee->session_open = true;
	}
	if (!tee->wsm_session_open) {
		status = KREE_CreateSession(tee->wsm_uuid, &tee->wsm_pHandle);
		if (status != TZ_RESULT_SUCCESS) {
			cleanup = KREE_CloseSession(tee->pHandle);
			if (cleanup == TZ_RESULT_SUCCESS)
				tee->session_open = false;
			return status;
		}
		tee->wsm_session_open = true;
	}
	return 0;
}

s32 cmdq_sec_mtee_close_session(struct cmdq_sec_mtee_context *tee)
{
	s32 status, first_error = 0;
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(tee->wsm_registered); index++)
		if (tee->wsm_registered[index])
			return -EBUSY;
	if (tee->cookie_registered) {
		status = KREE_UnregisterSharedmem(tee->wsm_pHandle, tee->mem_handle);
		if (status != TZ_RESULT_SUCCESS)
			return status;
		tee->cookie_registered = false;
	}
	if (tee->session_open) {
		status = KREE_CloseSession(tee->pHandle);
		if (status == TZ_RESULT_SUCCESS)
			tee->session_open = false;
		else
			first_error = status;
	}
	if (tee->wsm_session_open) {
		status = KREE_CloseSession(tee->wsm_pHandle);
		if (status == TZ_RESULT_SUCCESS)
			tee->wsm_session_open = false;
		else if (!first_error)
			first_error = status;
	}
	return first_error;
}

s32 cmdq_sec_mtee_execute_session(struct cmdq_sec_mtee_context *tee,
	u32 cmd, s32 timeout_ms, bool share_mem_ex, bool share_mem_ex2)
{
	union MTEEC_PARAM param[4] = {};
	u32 types;
	s32 status;

	if (!cmdq_mtee)
		return -EOPNOTSUPP;
	if (!tee->session_open || !tee->wsm_registered[0] ||
	    (share_mem_ex && !tee->wsm_registered[1]) ||
	    (share_mem_ex2 && !tee->wsm_registered[2]) ||
	    (cmd == 4 && !tee->cookie_registered))
		return -EINVAL;
	types = TZ_ParamTypes4(TZPT_VALUE_INOUT,
		share_mem_ex ? TZPT_VALUE_INOUT : TZPT_NONE,
		share_mem_ex2 ? TZPT_VALUE_INOUT : TZPT_NONE,
		cmd == 4 ? TZPT_VALUE_INOUT : TZPT_NONE);
	param[0].value.a = tee->wsm_handle;
	param[0].value.b = tee->wsm_param.size;
	param[1].value.a = tee->wsm_ex_handle;
	param[1].value.b = tee->wsm_ex_param.size;
	param[2].value.a = tee->wsm_ex2_handle;
	param[2].value.b = tee->wsm_ex2_param.size;
	param[3].value.a = tee->mem_handle;
	param[3].value.b = (u32)(unsigned long)tee->mem_param.buffer;
	status = KREE_TeeServiceCallPlus(tee->pHandle, cmd, types, param, 0);
	if (status != TZ_RESULT_SUCCESS)
		cmdq_err("MTEE service command:%u session:%#x error:%d", cmd, tee->pHandle, status);
	return status;
}

MODULE_LICENSE("GPL v2");
