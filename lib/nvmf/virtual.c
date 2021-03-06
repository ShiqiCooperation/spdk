/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include <stdlib.h>

#include "subsystem.h"
#include "session.h"
#include "request.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"
#include "spdk/scsi_spec.h"
#include "spdk/string.h"

#define MIN_KEEP_ALIVE_TIMEOUT 10000
#define MODEL_NUMBER "SPDK Virtual Controller"
#define FW_VERSION "FFFFFFFF"

/* read command dword 12 */
struct __attribute__((packed)) nvme_read_cdw12 {
	uint16_t	nlb;		/* number of logical blocks */
	uint16_t	rsvd	: 10;
	uint8_t		prinfo	: 4;	/* protection information field */
	uint8_t		fua	: 1;	/* force unit access */
	uint8_t		lr	: 1;	/* limited retry */
};

static void nvmf_virtual_set_dsm(struct spdk_nvmf_session *session)
{
	int i;

	for (i = 0; i < session->subsys->dev.virt.ns_count; i++) {
		struct spdk_bdev *bdev = session->subsys->dev.virt.ns_list[i];

		if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF,
				      "Subsystem%d Namespace %s does not support unmap - not enabling DSM\n",
				      i, bdev->name);
			return;
		}
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "All devices in Subsystem%d support unmap - enabling DSM\n",
		      session->subsys->num);
	session->vcdata.oncs.dsm = 1;
}

static void
nvmf_virtual_ctrlr_get_data(struct spdk_nvmf_session *session)
{
	struct spdk_nvmf_subsystem *subsys = session->subsys;

	memset(&session->vcdata, 0, sizeof(struct spdk_nvme_ctrlr_data));
	spdk_strcpy_pad(session->vcdata.fr, FW_VERSION, sizeof(session->vcdata.fr), ' ');
	spdk_strcpy_pad(session->vcdata.mn, MODEL_NUMBER, sizeof(session->vcdata.mn), ' ');
	session->vcdata.vid = 0x8086;
	session->vcdata.ssvid = 0x8086;
	spdk_strcpy_pad(session->vcdata.sn, subsys->dev.virt.sn, sizeof(session->vcdata.sn), ' ');
	session->vcdata.rab = 6;
	session->vcdata.ver.bits.mjr = 1;
	session->vcdata.ver.bits.mnr = 2;
	session->vcdata.ver.bits.ter = 1;
	session->vcdata.ctratt.host_id_exhid_supported = 1;
	session->vcdata.aerl = 0;
	session->vcdata.frmw.slot1_ro = 1;
	session->vcdata.frmw.num_slots = 1;
	session->vcdata.lpa.edlp = 1;
	session->vcdata.elpe = 127;
	session->vcdata.sqes.min = 0x06;
	session->vcdata.sqes.max = 0x06;
	session->vcdata.cqes.min = 0x04;
	session->vcdata.cqes.max = 0x04;
	session->vcdata.maxcmd = 1024;
	session->vcdata.nn = subsys->dev.virt.ns_count;
	session->vcdata.vwc.present = 1;
	session->vcdata.sgls.supported = 1;
	strncpy(session->vcdata.subnqn, session->subsys->subnqn, sizeof(session->vcdata.subnqn));
	nvmf_virtual_set_dsm(session);
}

static void
nvmf_virtual_ctrlr_poll_for_completions(struct spdk_nvmf_session *session)
{
	return;
}

static void
nvmf_virtual_ctrlr_complete_cmd(spdk_event_t event)
{
	struct spdk_bdev_io		*bdev_io = spdk_event_get_arg2(event);
	struct spdk_nvmf_request 	*req = spdk_event_get_arg1(event);
	enum spdk_bdev_io_status	status = bdev_io->status;
	struct spdk_nvme_cpl 		*response = &req->rsp->nvme_cpl;
	struct spdk_nvme_cmd 		*cmd = &req->cmd->nvme_cmd;

	if (cmd->opc == SPDK_NVME_OPC_DATASET_MANAGEMENT) {
		free(bdev_io->u.unmap.unmap_bdesc);
	}

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	} else {
		response->status.sc = SPDK_NVME_SC_SUCCESS;
	}
	spdk_nvmf_request_complete(req);
	spdk_bdev_free_io(bdev_io);
}

static int
nvmf_virtual_ctrlr_get_log_page(struct spdk_nvmf_request *req)
{
	uint8_t lid;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	if (req->data == NULL) {
		SPDK_ERRLOG("get log command with no buffer\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	lid = cmd->cdw10 & 0xFF;
	switch (lid) {
	case SPDK_NVME_LOG_ERROR:
	case SPDK_NVME_LOG_HEALTH_INFORMATION:
	case SPDK_NVME_LOG_FIRMWARE_SLOT:
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		SPDK_ERRLOG("Unsupported Get Log Page 0x%02X\n", lid);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
identify_ns(struct spdk_nvmf_subsystem *subsystem,
	    struct spdk_nvme_cmd *cmd,
	    struct spdk_nvme_cpl *rsp,
	    struct spdk_nvme_ns_data *nsdata)
{
	struct spdk_bdev *bdev;

	if (cmd->nsid > subsystem->dev.virt.ns_count || cmd->nsid == 0) {
		SPDK_ERRLOG("Identify Namespace for invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = subsystem->dev.virt.ns_list[cmd->nsid - 1];

	nsdata->nsze = bdev->blockcnt;
	nsdata->ncap = bdev->blockcnt;
	nsdata->nuse = bdev->blockcnt;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->lbaf[0].lbads = nvmf_u32log2(bdev->blocklen);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
identify_ctrlr(struct spdk_nvmf_session *session, struct spdk_nvme_ctrlr_data *cdata)
{
	*cdata = session->vcdata;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
identify_active_ns_list(struct spdk_nvmf_subsystem *subsystem,
			struct spdk_nvme_cmd *cmd,
			struct spdk_nvme_cpl *rsp,
			struct spdk_nvme_ns_list *ns_list)
{
	uint32_t i, num_ns, count = 0;

	if (cmd->nsid >= 0xfffffffeUL) {
		SPDK_ERRLOG("Identify Active Namespace List with invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	num_ns = subsystem->dev.virt.ns_count;

	for (i = 1; i <= num_ns; i++) {
		if (i <= cmd->nsid) {
			continue;
		}
		ns_list->ns_list[count++] = i;
		if (count == sizeof(*ns_list) / sizeof(uint32_t)) {
			break;
		}
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_virtual_ctrlr_identify(struct spdk_nvmf_request *req)
{
	uint8_t cns;
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem *subsystem = session->subsys;

	if (req->data == NULL || req->length < 4096) {
		SPDK_ERRLOG("identify command with invalid buffer\n");
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	memset(req->data, 0, req->length);

	cns = cmd->cdw10 & 0xFF;
	switch (cns) {
	case SPDK_NVME_IDENTIFY_NS:
		return identify_ns(subsystem, cmd, rsp, req->data);
	case SPDK_NVME_IDENTIFY_CTRLR:
		return identify_ctrlr(session, req->data);
	case SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST:
		return identify_active_ns_list(subsystem, cmd, rsp, req->data);
	default:
		SPDK_ERRLOG("Identify command with unsupported CNS 0x%02x\n", cns);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
nvmf_virtual_ctrlr_get_features(struct spdk_nvmf_request *req)
{
	uint8_t feature;
	uint32_t nr_io_queues;
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	feature = cmd->cdw10 & 0xff; /* mask out the FID value */
	switch (feature) {
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Number of Queues\n");
		nr_io_queues = session->max_connections_allowed - 1;
		/* Number of IO queues has a zero based value */
		response->cdw0 = ((nr_io_queues - 1) << 16) |
				 (nr_io_queues - 1);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		response->cdw0 = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		response->cdw0 = session->kato;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		SPDK_ERRLOG("get features command with invalid code\n");
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
nvmf_virtual_ctrlr_set_features(struct spdk_nvmf_request *req)
{
	uint8_t feature;
	uint32_t nr_io_queues = 0;
	struct spdk_nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	feature = cmd->cdw10 & 0xff; /* mask out the FID value */
	switch (feature) {
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Number of Queues, cdw11 0x%x\n", cmd->cdw11);
		nr_io_queues = session->max_connections_allowed - 1;
		/* verify that the contoller is ready to process commands */
		if (session->num_connections > 1) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Queue pairs already active!\n");
			response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		} else {
			/* Number of IO queues has a zero based value */
			response->cdw0 = ((nr_io_queues - 1) << 16) |
					 (nr_io_queues - 1);
		}
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		if (cmd->cdw11 == 0) {
			response->status.sc = SPDK_NVME_SC_KEEP_ALIVE_INVALID;
		} else if (cmd->cdw11 < MIN_KEEP_ALIVE_TIMEOUT) {
			session->kato = MIN_KEEP_ALIVE_TIMEOUT;
		} else {
			session->kato = cmd->cdw11;
		}
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		SPDK_ERRLOG("set features command with invalid code\n");
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
nvmf_virtual_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;

	switch (cmd->opc) {
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		return nvmf_virtual_ctrlr_get_log_page(req);
	case SPDK_NVME_OPC_IDENTIFY:
		return nvmf_virtual_ctrlr_identify(req);
	case SPDK_NVME_OPC_GET_FEATURES:
		return nvmf_virtual_ctrlr_get_features(req);
	case SPDK_NVME_OPC_SET_FEATURES:
		return nvmf_virtual_ctrlr_set_features(req);
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Async Event Request\n");
		/* TODO: Just release the request as consumed. AER events will never
		 * be triggered. */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_RELEASE;
	case SPDK_NVME_OPC_KEEP_ALIVE:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Keep Alive\n");
		/*
		  To handle keep alive just clear or reset the
		  session based keep alive duration counter.
		  When added, a separate timer based process
		  will monitor if the time since last recorded
		  keep alive has exceeded the max duration and
		  take appropriate action.
		*/
		//session->keep_alive_timestamp = ;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;

	case SPDK_NVME_OPC_CREATE_IO_SQ:
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		SPDK_ERRLOG("Admin opc 0x%02X not allowed in NVMf\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		SPDK_ERRLOG("Unsupported admin command\n");
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

}

static int
nvmf_virtual_ctrlr_rw_cmd(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			  struct spdk_nvmf_request *req)
{
	uint64_t lba_address;
	uint64_t blockcnt;
	uint64_t io_bytes;
	uint64_t offset;
	uint64_t llen;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct nvme_read_cdw12 *cdw12 = (struct nvme_read_cdw12 *)&cmd->cdw12;

	blockcnt = bdev->blockcnt;
	lba_address = cmd->cdw11;
	lba_address = (lba_address << 32) + cmd->cdw10;
	offset = lba_address * bdev->blocklen;
	llen = cdw12->nlb + 1;

	if (lba_address >= blockcnt || llen > blockcnt || lba_address > (blockcnt - llen)) {
		SPDK_ERRLOG("end of media\n");
		response->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	io_bytes = llen * bdev->blocklen;
	if (io_bytes > req->length) {
		SPDK_ERRLOG("Read/Write NLB > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->opc == SPDK_NVME_OPC_READ) {
		spdk_trace_record(TRACE_NVMF_LIB_READ_START, 0, 0, (uint64_t)req, 0);
		if (spdk_bdev_read(bdev, ch, req->data, offset, req->length, nvmf_virtual_ctrlr_complete_cmd,
				   req) == NULL) {
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else {
		spdk_trace_record(TRACE_NVMF_LIB_WRITE_START, 0, 0, (uint64_t)req, 0);
		if (spdk_bdev_write(bdev, ch, req->data, offset, req->length, nvmf_virtual_ctrlr_complete_cmd,
				    req) == NULL) {
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;

}

static int
nvmf_virtual_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			     struct spdk_nvmf_request *req)
{

	uint64_t nbytes;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	nbytes = bdev->blockcnt * bdev->blocklen;
	if (spdk_bdev_flush(bdev, ch, 0, nbytes, nvmf_virtual_ctrlr_complete_cmd, req) == NULL) {
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
nvmf_virtual_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_io_channel *ch,
			   struct spdk_nvmf_request *req)
{
	int i;
	uint32_t attribute;
	uint16_t nr;
	struct spdk_scsi_unmap_bdesc *unmap;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	bool async = false;

	nr = ((cmd->cdw10 & 0x000000ff) + 1);
	if (nr * sizeof(struct spdk_nvme_dsm_range) > req->length) {
		SPDK_ERRLOG("Dataset Management number of ranges > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	attribute = cmd->cdw11 & 0x00000007;
	if (attribute & SPDK_NVME_DSM_ATTR_DEALLOCATE) {
		struct spdk_nvme_dsm_range *dsm_range = (struct spdk_nvme_dsm_range *)req->data;
		unmap = calloc(nr, sizeof(*unmap));
		if (unmap == NULL) {
			SPDK_ERRLOG("memory allocation failure\n");
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		for (i = 0; i < nr; i++) {
			to_be64(&unmap[i].lba, dsm_range[i].starting_lba);
			to_be32(&unmap[i].block_count, dsm_range[i].length);
		}
		if (spdk_bdev_unmap(bdev, ch, unmap, nr, nvmf_virtual_ctrlr_complete_cmd, req) == NULL) {
			free(unmap);
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
		async = true;
	}

	if (async) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_virtual_ctrlr_process_io_cmd(struct spdk_nvmf_request *req)
{
	uint32_t nsid;
	struct spdk_bdev *bdev;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_subsystem *subsystem = req->conn->sess->subsys;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	nsid = cmd->nsid;

	if (nsid > subsystem->dev.virt.ns_count || nsid == 0) {
		SPDK_ERRLOG("Unsuccessful query for nsid %u\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = subsystem->dev.virt.ns_list[nsid - 1];
	ch = subsystem->dev.virt.ch[nsid - 1];
	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
		return nvmf_virtual_ctrlr_rw_cmd(bdev, ch, req);
	case SPDK_NVME_OPC_FLUSH:
		return nvmf_virtual_ctrlr_flush_cmd(bdev, ch, req);
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		return nvmf_virtual_ctrlr_dsm_cmd(bdev, ch, req);
	default:
		SPDK_ERRLOG("Unsupported IO command opc: %x\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static void
nvmf_virtual_ctrlr_detach(struct spdk_nvmf_subsystem *subsystem)
{
	return;
}

const struct spdk_nvmf_ctrlr_ops spdk_nvmf_virtual_ctrlr_ops = {
	.ctrlr_get_data			= nvmf_virtual_ctrlr_get_data,
	.process_admin_cmd		= nvmf_virtual_ctrlr_process_admin_cmd,
	.process_io_cmd			= nvmf_virtual_ctrlr_process_io_cmd,
	.poll_for_completions		= nvmf_virtual_ctrlr_poll_for_completions,
	.detach				= nvmf_virtual_ctrlr_detach,
};
