/* Copyright 2013 Freescale Semiconductor, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * Neither the name of Freescale Semiconductor nor the
 * names of its contributors may be used to endorse or promote products
 * derived from this software without specific prior written permission.
 *
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") as published by the Free Software
 * Foundation, either version 2 of that License or (at your option) any
 * later version.
 *
 * THIS SOFTWARE IS PROVIDED BY Freescale Semiconductor ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Freescale Semiconductor BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <usdpaa/of.h>
#include <internal/of.h>
#include <usdpaa/dma_mem.h>
#include <usdpaa/pci_ep_vfio.h>
#include <usdpaa/fsl_dma.h>
#include <readline.h>  /* libedit */
#include <argp.h>

#include "atb_clock.h"
#include "pciep_dma_cfg.h"

#define ENABLE_CHAIN_MODE
#define ENABLE_MULTI_CHAIN_MODE
#undef LOCAL_MEM_TEST
#define ENABLE_PCIEP_DEBUG
#define DMA_CH_USE 16 /* How many channel will be used */


enum worker_msg_type {
	worker_msg_none = 0,
	worker_msg_quit,
};

struct worker {
	volatile enum worker_msg_type msg;
	void *pvt;
	struct list_head node;
	int cpu;
	pthread_t id;
	int status;
};

struct pciep_dma_dev {
	char *name;
	struct pcipf_dev *pf;
	int type;
	int idx;
	struct pcidma_config *config;
	void *local_buffer;
	void *remote_buffer;
	void *msix_trap;
	struct pci_ep_win *msix_win;
	struct pci_ep_win *config_win;
	struct pci_ep_win *buffer_win;
	struct pci_ep_win *out_mem_win;
	struct pci_ep *ep;
	struct worker *worker;
};

struct pcipf_dev {
	int idx;
	struct pci_controller *controller;
	int vf_num;
	int ep_num;
	struct pciep_dma_dev *pcidma;
	void *reg;
	void *msix_trap;
	void *config;
	void *local_buffer;
	void *vf_msix;
	void *vf_config;
	void *vf_local_buffer;
};

struct pci_controller {
	int idx;
	int pf_num;
	struct pcipf_dev *pf;
};

#ifdef ENABLE_PCIEP_DEBUG
	#define PCIEP_DBG(fmt, args...) fprintf(stderr, fmt, ##args);
#else
	#define PCIEP_DBG(fmt, args...)
#endif

#define MAX_PF_NUM 2
#define MAX_VF_NUM 64
#define DMA_MEM_SIZE (512 * 1024 * 1024) /* 512MB */
#define MSIX_WIN_SIZE (8 * 1024) /* 8K */
#define CONFIG_WIN_SIZE (4 * 1024) /* 4K */
#define BUFFER_WIN_SIZE (2 * 1024 * 1024) /* 2M */
#define BUFFER_OFFSET_MASK ((u64)BUFFER_WIN_SIZE - 1)
#define CONFIG_SPACE_SIZE 0x1000

#define MSIX_TD_TYPE_PF 0
#define MSIX_TD_TYPE_VF 1
#define MSIX_TD_TYPE_SHIFT 31
#define MSIX_TD_VF_SHIFT 16
#define MSIX_TD_ENTRY_SHIFT 8
#define MSIX_TD_OP_TRIGGER 0
#define MSIX_TD_OP_CLEAR_PENDING 1

const char pciep_prompt[] = "pcidma> ";
static int64_t atb_multiplier;
static unsigned long ncpus;
static unsigned long cpu_idx;

static struct dma_ch *dmadevs[DMA_CH_USE];
static unsigned int ch_used = DMA_CH_USE;

static LIST_HEAD(workers);

struct pci_controller *host;

static int worker_reap(struct worker *worker);

static int msg_post(struct worker *worker, enum worker_msg_type msg)
{
	worker->msg = msg;
	while (worker->msg != worker_msg_none) {
		if (!worker_reap(worker))
			/* The worker is already gone */
			return -EIO;
		pthread_yield();
	}
	return 0;
}

static int process_msg(struct worker *worker)
{
	int ret = 1;

	if (worker->msg == worker_msg_none)
		return 1;
	/* Quit */
	else if (worker->msg == worker_msg_quit)
		ret = 0;

	/* Release ourselves and the CLI thread from this message */
	worker->msg = worker_msg_none;

	return ret;
}

void pcidma_send_msix(struct pciep_dma_dev *pcidma, int idx)
{
	int type;
	int vf_idx;

	if (pcidma->type == PCI_EP_TYPE_PF) {
		type = MSIX_TD_TYPE_PF;
		vf_idx = 0;
	} else {
		type = MSIX_TD_TYPE_VF;
		vf_idx = pcidma->idx - 1;
	}

	out_be32(pcidma->msix_trap, type << MSIX_TD_TYPE_SHIFT |
		 vf_idx << MSIX_TD_VF_SHIFT | idx << MSIX_TD_ENTRY_SHIFT |
		 MSIX_TD_OP_TRIGGER);
}

static u64 dma_direct_mode_test(struct dma_ch *dmadev,
				dma_addr_t src_phys, dma_addr_t dest_phys,
				uint32_t len, uint32_t loop)
{
	struct atb_clock *atb_clock = NULL;
	int i;
	u64 result64;

	atb_clock = malloc(sizeof(*atb_clock));
	if (!atb_clock) {
		error(0, 0, "failed to initialize atb_clock!\n");
		return 0;
	}
	atb_clock_init(atb_clock);

	fsl_dma_chan_basic_direct_init(dmadev);

	fsl_dma_chan_bwc(dmadev, DMA_BWC_1024);

	for (i = 0; i < loop; i++) {
		atb_clock_start(atb_clock);
		fsl_dma_direct_start(dmadev, src_phys, dest_phys, len);
		if (fsl_dma_wait(dmadev) < 0) {
			error(0, 0, "dma task error!\n");
			goto _err;
		}
		atb_clock_stop(atb_clock);
	}

	result64 = len * 8 * loop * atb_multiplier /
		atb_clock_total(atb_clock);
	PCIEP_DBG("\ttest direct mode result:%lldMbps\n", result64/1024/1024);
	return result64;
_err:
	free(atb_clock);
	return 0;
}

static int dma_chain_mode_test(struct dma_ch *dmadev, dma_addr_t src_phys,
				dma_addr_t dest_phys, uint32_t len)
{
	struct atb_clock *atb_clock = NULL;
	struct dma_link_setup_data *link_data = NULL;
	struct dma_link_dsc *link_dsc = NULL;
	u64 link_dsc_phy, result64 = 0;
	int max_count, count;
	int i;

	max_count = 32 * 1024; /* BUFFER_WIN_SIZE / len; */

	atb_clock = malloc(sizeof(*atb_clock));
	if (!atb_clock) {
		error(0, 0, "failed to initialize atb_clock!\n");
		return 0;
	}
	atb_clock_init(atb_clock);

	link_data = malloc(sizeof(*link_data) * max_count);
	if (!link_data) {
		error(0, 0,"can not allocate memeory for link_data\n");
		goto _err;
	}

	link_dsc = __dma_mem_memalign(BUFFER_WIN_SIZE, BUFFER_WIN_SIZE);
	if (!link_dsc) {
		error(0, 0, "failed to allocate dma mem for link_dsc\n");
		goto _err;
	}
	link_dsc_phy = __dma_mem_vtop(link_dsc);

	fsl_dma_chan_bwc(dmadev, DMA_BWC_1024);
	for (count = 1; count <= max_count; count = count * 2) {
		for (i = 0; i < count; i++) {
			link_data[i].byte_count = len;
			link_data[i].src_addr = src_phys /* + i * len */;
			link_data[i].dst_addr = dest_phys /* + i * len */;
			link_data[i].dst_snoop_en = 1;
			link_data[i].src_snoop_en = 1;
			link_data[i].dst_nlwr = 0;
			link_data[i].dst_stride_en = 0;
			link_data[i].src_stride_en = 0;
			link_data[i].dst_stride_dist = 0;
			link_data[i].src_stride_dist = 0;
			link_data[i].dst_stride_size = 0;
			link_data[i].src_stride_size = 0;
			link_data[i].err_interrupt_en = 0;
			link_data[i].seg_interrupt_en = 0;
			link_data[i].link_interrupt_en = 0;
		}
		fsl_dma_chain_link_build(link_data, link_dsc,
				link_dsc_phy, count);

		atb_clock_init(atb_clock);
		atb_clock_start(atb_clock);
		fsl_dma_chain_basic_start(dmadev, link_data, link_dsc_phy);
		if (fsl_dma_wait(dmadev) < 0) {
			error(0, 0, "dma task error!\n");
			goto _err;
		}
		atb_clock_stop(atb_clock);

		result64 = (u64)len * 8 * count * atb_multiplier /
				atb_clock_total(atb_clock);
		PCIEP_DBG("\ttest chain mode(%d dscs) result:%lldMbps\n",
			  count, result64/1024/1024);
	}

	return result64;
_err:
	free(atb_clock);
	free(link_data);
	if (link_dsc)
		__dma_mem_free(link_dsc);
	return 0;
}

static int dma_multichain_mode_test(struct dma_ch *dmadev, dma_addr_t src_phys,
				dma_addr_t dest_phys, uint32_t len)
{
	struct atb_clock *atb_clock = NULL;
	struct dma_link_setup_data *link_data = NULL;
	struct dma_link_dsc *link_dsc = NULL;
	u64 link_dsc_phy, result64 = 0;
	int max_count, count;
	int i, j;

	max_count = 32 * 1024; /* BUFFER_WIN_SIZE / len; */

	atb_clock = malloc(sizeof(*atb_clock));
	if (!atb_clock) {
		error(0, 0, "failed to initialize atb_clock!\n");
		return 0;
	}
	atb_clock_init(atb_clock);

	link_data = malloc(sizeof(*link_data) * max_count);
	if (!link_data) {
		error(0, 0,"can not allocate memeory for link_data\n");
		goto _err;
	}

	link_dsc = __dma_mem_memalign(BUFFER_WIN_SIZE, BUFFER_WIN_SIZE);
	if (!link_dsc) {
		error(0, 0, "failed to allocate dma mem for link_dsc\n");
		goto _err;
	}
	link_dsc_phy = __dma_mem_vtop(link_dsc);
	for (i = 0; i < ch_used; i++)
		fsl_dma_chan_bwc(dmadevs[i], DMA_BWC_1024);

	for (count = 1; count <= max_count; count = count * 2) {
		for (i = 0; i < count; i++) {
			link_data[i].byte_count = len;
			link_data[i].src_addr = src_phys /* + i * len */;
			link_data[i].dst_addr = dest_phys /* + i * len */;
			link_data[i].dst_snoop_en = 1;
			link_data[i].src_snoop_en = 1;
			link_data[i].dst_nlwr = 0;
			link_data[i].dst_stride_en = 0;
			link_data[i].src_stride_en = 0;
			link_data[i].dst_stride_dist = 0;
			link_data[i].src_stride_dist = 0;
			link_data[i].dst_stride_size = 0;
			link_data[i].src_stride_size = 0;
			link_data[i].err_interrupt_en = 0;
			link_data[i].seg_interrupt_en = 0;
			link_data[i].link_interrupt_en = 0;
		}
		fsl_dma_chain_link_build(link_data, link_dsc,
				link_dsc_phy, count);

		for (j = 1; j <= ch_used; j++) {
			atb_clock_init(atb_clock);
			atb_clock_start(atb_clock);
			for (i = 0; i < j; i++)
				fsl_dma_chain_basic_start(dmadevs[i], link_data, link_dsc_phy);
			for (i = 0; i < j; i++) {
				if (fsl_dma_wait(dmadevs[i]) < 0) {
					error(0, 0, "dma channel %d task error!\n", i);
					goto _err;
				}
			}
			atb_clock_stop(atb_clock);

			result64 = (u64)len * 8 * count * j * atb_multiplier /
				atb_clock_total(atb_clock);
			PCIEP_DBG("\ttest chain mode(%d dscs, %d channels) result:%lldMbps\n",
			  count, j, result64/1024/1024);
		}
	}

	return result64;
_err:
	free(atb_clock);
	free(link_data);
	if (link_dsc)
		__dma_mem_free(link_dsc);
	return 0;
}

static int pcidma_test(struct pciep_dma_dev *pcidma, struct dma_ch *dmadev)
{
	volatile struct pcidma_config *config;
	volatile struct rw_config *rwcfg;
	uint64_t bar, offset, local, remote, src, dest;
	void *buf = NULL;

	config = pcidma->config;
	rwcfg = &config->rwcfg;

	bar = rwcfg->bar64;

	if (rwcfg->loop == 0 || rwcfg->size == 0 ||
	    rwcfg->size > BUFFER_WIN_SIZE)
		goto _err;

	offset = bar & BUFFER_OFFSET_MASK;
	pcidma->out_mem_win->pci_addr = bar & (~BUFFER_OFFSET_MASK);
	pcidma->out_mem_win->size = BUFFER_WIN_SIZE;
	pcidma->out_mem_win->attr = 0; /* Use default setting */
	vfio_pci_ep_set_win(pcidma->ep, pcidma->out_mem_win);

	remote = pcidma->out_mem_win->cpu_addr + offset;

	buf = __dma_mem_memalign(BUFFER_WIN_SIZE, BUFFER_WIN_SIZE);
	if (!buf) {
		error(0, 0, "failed to allocate dma mem %dB\n", rwcfg->size);
		goto _err;
	}
	memset(buf, 0xa5, rwcfg->size);
	local = __dma_mem_vtop(buf);

#ifdef LOCAL_MEM_TEST
	buf = __dma_mem_memalign(BUFFER_WIN_SIZE, BUFFER_WIN_SIZE);
	if (!buf) {
		error(0, 0, "failed to allocate dma mem %dB\n", rwcfg->size);
		goto _err;
	}
	remote = __dma_mem_vtop(buf);
#endif

	if (rwcfg->type == RW_TYPE_WRITE) {
		dest = remote;
		src = local;
	} else if (rwcfg->type == RW_TYPE_READ) {
		dest = local;
		src = remote;
	} else
		goto _err;

	PCIEP_DBG("\nstart a test\n");
	PCIEP_DBG("\tsrc addr:%llx dest addr:%llx loop:0x%d size:%dB\n",
		  src, dest, rwcfg->loop, rwcfg->size);

	rwcfg->result64 = dma_direct_mode_test(dmadev, src, dest,
					       rwcfg->size, rwcfg->loop);
#ifdef ENABLE_CHAIN_MODE
	dma_chain_mode_test(dmadev, src, dest, rwcfg->size);
#endif
#ifdef ENABLE_MULTI_CHAIN_MODE
	dma_multichain_mode_test(dmadev, src, dest, rwcfg->size);
#endif

	__dma_mem_free(buf);
	buf = NULL;
	if (rwcfg->result64)
		return PCIDMA_STATUS_DONE;

_err:
	if (buf)
		__dma_mem_free(buf);

	return PCIDMA_STATUS_ERROR;
}

static void *worker_fn(void *__worker)
{
	struct worker *worker = __worker;
	cpu_set_t cpuset;
	int status;
	struct pciep_dma_dev *pcidma = NULL;
	volatile struct pcidma_config *config;

	pcidma = worker->pvt;
	config = pcidma->config;

	fprintf(stderr, "Starting a %s\'s test thread on cpu%d\n",
		pcidma->name, worker->cpu);

	/* Set this cpu-affinity */
	CPU_ZERO(&cpuset);
	CPU_SET(worker->cpu, &cpuset);
	status = pthread_setaffinity_np(worker->id, sizeof(cpu_set_t), &cpuset);
	if (status != 0) {
		error(0, -status, "pthread_setaffinity_np(%d) failed\n",
			worker->cpu);
		goto end;
	}

	/*
	 * check_msg returns 1 when no message,
	 * otherwise returns value from process_msg: 0 : quit
	 */
	while (process_msg(worker)) {
		if (config->command == PCIDMA_CMD_START) {
			worker->status = config->status = PCIDMA_STATUS_BUSY;
			status = pcidma_test(pcidma, dmadevs[0]);
			worker->status = config->status = status;
			config->command = PCIDMA_CMD_NONE;
		}
	}

end:
	fprintf(stderr, "Leaving %s\'s test thread on cpu%d\n",
		pcidma->name, worker->cpu);

	return NULL;
}

static int worker_new(struct pciep_dma_dev *pcidma)
{
	struct worker *worker;
	int err;

	if (pcidma->worker) {
		fprintf(stderr, "The %s\'s test thread has been created\n",
			pcidma->name);
		return 0;
	}

	err = posix_memalign((void **)&worker, L1_CACHE_BYTES, sizeof(*worker));
	if (err)
		return -ENOMEM;

	memset(worker, 0, sizeof(*worker));
	pcidma->worker = worker;
	worker->cpu = (++cpu_idx) % ncpus;
	worker->pvt = pcidma;
	worker->msg = worker_msg_none;
	INIT_LIST_HEAD(&worker->node);
	err = pthread_create(&worker->id, NULL, worker_fn, worker);
	if (err) {
		free(worker);
		pcidma->worker = NULL;
		error(0, 0, "failed to create worker for device:%s\n",
			pcidma->name);
		return -EINVAL;
	}

	list_add_tail(&worker->node, &workers);

	return 0;
}

static int workers_auto_new(int worker_total)
{
	int pf_idx, ep_idx, worker_num = 0;
	struct pcipf_dev *pf;
	struct pciep_dma_dev *pcidma;

	if (!host)
		return 0;

	if (worker_total <= 0)
		return 0;

	for (pf_idx = 0; pf_idx < host->pf_num; pf_idx++) {
		pf = &host->pf[pf_idx];
		for (ep_idx = 0; ep_idx < pf->ep_num; ep_idx++) {
			pcidma = &pf->pcidma[ep_idx];
			worker_new(pcidma);
			worker_num++;
			if (worker_num == worker_total)
				return worker_total;
		}
	}

	return worker_num;
}

static int worker_reap(struct worker *worker)
{
	struct pciep_dma_dev *pcidma = worker->pvt;

	if (pthread_tryjoin_np(worker->id, NULL))
		return -EBUSY;

	if (!list_empty(&worker->node))
		list_del(&worker->node);

	free(worker);
	pcidma->worker = NULL;
	return 0;
}

static void worker_free(struct worker *worker)
{
	struct pciep_dma_dev *pcidma;
	int err;

	if (!worker)
		return;

	pcidma = worker->pvt;

	msg_post(worker, worker_msg_quit);

	err = pthread_join(worker->id, NULL);
	if (err) {
		/* Leak, but warn */
		error(0, 0, "Failed to join thread id:%u for device:%s\n",
			(uint32_t)worker->id, pcidma->name);
		return;
	}

	list_del(&worker->node);
	free(worker);
	pcidma->worker = NULL;
}

static int pciep_iw_set(struct pci_ep *ep, struct pci_ep_win *win,
			uint64_t phy, uint64_t size)
{
	win->cpu_addr = phy;
	win->size = size;
	win->attr = 0;
	return vfio_pci_ep_set_win(ep, win);
}

static int pcidma_init(struct pcipf_dev *pf, int i)
{
	struct pciep_dma_dev *pcidma;

	pcidma = &pf->pcidma[i];
	pcidma->idx = i;
	pcidma->pf = pf;
	pcidma->ep = vfio_pci_ep_open(pcidma->pf->controller->idx,
					pcidma->pf->idx,
					pcidma->idx);
	if (!pcidma->ep)
		return -EINVAL;

	pcidma->type = pcidma->ep->info.type;
	pcidma->name = pcidma->ep->name;
	fprintf(stderr, "Initialized %s\n", pcidma->name);

	pcidma->msix_win = &pcidma->ep->iw[PCI_EP_WIN1_INDEX];
	pcidma->config_win = &pcidma->ep->iw[PCI_EP_WIN2_INDEX];
	pcidma->buffer_win = &pcidma->ep->iw[PCI_EP_WIN3_INDEX];

	if (pcidma->type == PCI_EP_TYPE_PF)
		pcidma->out_mem_win = &pcidma->ep->ow[PCI_EP_WIN1_INDEX];
	else
		pcidma->out_mem_win = &pcidma->ep->ow[PCI_EP_WIN0_INDEX];

	return 0;
}

static int pcidma_setup(struct pciep_dma_dev *pcidma)
{
	struct pcipf_dev *pf = pcidma->pf;

	pcidma->remote_buffer =
		vfio_pci_ep_map_win(pcidma->ep, pcidma->out_mem_win,
				    0, BUFFER_WIN_SIZE);

	if (pcidma->type == PCI_EP_TYPE_PF) {
		pcidma->config = pf->config;
		pcidma->local_buffer = pf->local_buffer;
	} else {
		int idx = pcidma->idx - 1;

		pcidma->config = pf->vf_config + CONFIG_WIN_SIZE * idx;
		pcidma->local_buffer = pf->vf_local_buffer +
				       BUFFER_WIN_SIZE * idx;
	}

	memset(pcidma->config, 0, CONFIG_WIN_SIZE);

	pcidma->msix_trap = pf->msix_trap + ((pcidma->idx * 64) & 0xfc0);

	return 0;
}

static void pcidma_free(struct pciep_dma_dev *pcidma)
{
	if (!pcidma)
		return;

	worker_free(pcidma->worker);

	if (pcidma->remote_buffer)
		munmap(pcidma->remote_buffer, BUFFER_WIN_SIZE);

	vfio_pci_ep_close(pcidma->ep);
}

static int pcipf_setup(struct pcipf_dev *pf)
{
	struct pciep_dma_dev *pcidma;
	struct pci_ep *ep;
	int i;

	if (!pf || !pf->pcidma)
		return -EINVAL;

	pcidma = &pf->pcidma[0];
	ep = pcidma->ep;

	if (pf->vf_num > 0) {
		pf->vf_local_buffer =
			__dma_mem_memalign(BUFFER_WIN_SIZE * pf->vf_num,
					   BUFFER_WIN_SIZE * pf->vf_num);
		if (!pf->vf_local_buffer) {
			error(0, 0, "failed to requst dma memory for VF buf\n");
			return -ENOMEM;
		}

		pf->vf_config =
			__dma_mem_memalign(CONFIG_WIN_SIZE * pf->vf_num,
					   CONFIG_WIN_SIZE * pf->vf_num);
		if (!pf->vf_config) {
			error(0, 0, "failed to requst dma memory for VF cfg\n");
			return -ENOMEM;
		}

		pciep_iw_set(ep, &ep->vfiw[PCI_EP_WIN3_INDEX],
			     __dma_mem_vtop(pf->vf_local_buffer),
			     BUFFER_WIN_SIZE);

		pciep_iw_set(ep, &ep->vfiw[PCI_EP_WIN2_INDEX],
			     __dma_mem_vtop(pf->vf_config),
			     CONFIG_WIN_SIZE);
	}

	pf->reg = vfio_pci_ep_map_win(ep, &ep->reg, 0, ep->reg.size);

	pf->config = __dma_mem_memalign(CONFIG_WIN_SIZE, CONFIG_WIN_SIZE);
	if (!pf->config) {
		error(0, 0, "failed to requst dma memory for config");
		return -ENOMEM;
	}

	pf->local_buffer =
		__dma_mem_memalign(BUFFER_WIN_SIZE, BUFFER_WIN_SIZE);
	if (!pf->local_buffer) {
		error(0, 0, "failed to requst dma memory for local_buffer");
		return -ENOMEM;
	}

	pciep_iw_set(pcidma->ep, pcidma->config_win,
			__dma_mem_vtop(pf->config), CONFIG_WIN_SIZE);

	pciep_iw_set(pcidma->ep, pcidma->buffer_win,
			__dma_mem_vtop(pf->local_buffer), BUFFER_WIN_SIZE);

	if (pcidma->ep->info.msix_enable) {
		pf->msix_trap = vfio_pci_ep_map_win(ep, &ep->msixow,
						    0, ep->msixow.size);
		if (!pf->msix_trap) {
			error(0, 0, "failed to map MSIX OW\n");
			return -ENOMEM;
		}
	}

	for (i = 0; i < pf->ep_num; i++)
		pcidma_setup(&pf->pcidma[i]);

	return 0;
}

static int pcipf_init(struct pcipf_dev *pf)
{
	char path[PATH_MAX];
	struct stat st;
	int i;

	for (i = 1; i < MAX_VF_NUM; i++) {
		snprintf(path, sizeof(path),
			 "/sys/class/pci_ep/pci%d-pf%d-vf%d",
			 pf->controller->idx, pf->idx, i);
		if (stat(path, &st) < 0)
			break;
	}

	pf->vf_num = i;
	pf->ep_num = pf->vf_num + 1;
	PCIEP_DBG("pf%d\'s vf number is %d\n", pf->idx, pf->vf_num);

	pf->pcidma = malloc(pf->ep_num * sizeof(*pf->pcidma));
	if (!pf->pcidma) {
		error(0, 0, "failed to allocate memory for pcidma\n");
		return -ENOMEM;
	}
	memset(pf->pcidma, 0, pf->ep_num * sizeof(*pf->pcidma));

	for (i = 0; i < pf->ep_num; i++)
		if (pcidma_init(pf, i))
			return -EINVAL;

	return 0;
}

static void pcipf_free(struct pcipf_dev *pf)
{
	int i;

	if (!pf || !pf->pcidma)
		return;

	for (i = 0; i < pf->ep_num; i++)
		pcidma_free(&pf->pcidma[i]);

	if (pf->config)
		__dma_mem_free(pf->config);
	if (pf->local_buffer)
		__dma_mem_free(pf->local_buffer);

	if (pf->vf_config)
		__dma_mem_free(pf->vf_config);
	if (pf->vf_local_buffer)
		__dma_mem_free(pf->vf_local_buffer);

	free(pf->pcidma);
}

static int pci_controller_init(struct  pci_controller *controller, int idx)
{
	char path[PATH_MAX];
	struct stat st;
	int i, err;

	controller->idx = idx;

	for (i = 0; i < MAX_PF_NUM; i++) {
		snprintf(path, sizeof(path),
			 "/sys/class/pci_ep/pci%d-pf%d", idx, i);

		if (stat(path, &st) < 0)
			break;
	}

	controller->pf_num = i;
	if (controller->pf_num < 0)
		return -EINVAL;

	PCIEP_DBG("PCI controller%d\'s pf number is %d\n",
		  controller->idx, controller->pf_num);

	controller->pf = malloc(controller->pf_num * sizeof(*controller->pf));
	if (!controller->pf) {
		error(0, 0, "failed to allocate memory for pf\n");
		return -ENOMEM;
	}
	memset(controller->pf, 0, controller->pf_num * sizeof(*controller->pf));

	for (i = 0; i < controller->pf_num; i++) {
		struct pcipf_dev *pf;

		pf = &controller->pf[i];
		pf->controller = controller;
		pf->idx = i;
		err = pcipf_init(&controller->pf[i]);
		if (err)
			return err;
		err = pcipf_setup(&controller->pf[i]);
		if (err)
			return err;
	}

	return 0;
}

static void pci_controller_free(struct  pci_controller *controller)
{
	int i;

	if (!controller)
		return;

	if (controller->pf) {
		for (i = 0; i < controller->pf_num; i++)
			pcipf_free(&controller->pf[i]);
		free(controller->pf);
	}

	free(controller);
}

/* can implement new CLI functions using the cli_cmd() macro. */
typedef int (*cli_handle_t)(int argc, char *argv[]);
struct cli_table_entry {
	const char *cmd;
	const cli_handle_t handle;
};
#define cli_cmd(cmd, handle)					\
	const struct cli_table_entry cli_table_entry_##cmd	\
	__attribute__((used, section(".rodata.cli_table")))	\
	= {__stringify(cmd), handle}

extern const struct cli_table_entry cli_table_start[], cli_table_end[];

#define foreach_cli_table_entry(cli_cmd)				\
	for (cli_cmd = cli_table_start; cli_cmd < cli_table_end; cli_cmd++)

static int pciep_cli_help(int argc, char *argv[])
{
	const struct cli_table_entry *cli_cmd;

	puts("Available commands:");
	foreach_cli_table_entry(cli_cmd)
		fprintf(stderr, "%s\n", cli_cmd->cmd);

	puts("");

	return argc != 1 ? -EINVAL : 0;
}

static struct pciep_dma_dev *pciep_find(int pf_idx, int ep_idx)
{
	struct pcipf_dev *pf;

	if (!host)
		return NULL;

	if (pf_idx >= host->pf_num) {
		fprintf(stderr, "Max pf index is %d\n", host->pf_num);
		return NULL;
	}

	pf = &host->pf[pf_idx];

	if (ep_idx >= pf->ep_num) {
		fprintf(stderr, "Max VF index is %d\n", pf->ep_num);
		return NULL;
	}

	return &pf->pcidma[ep_idx];
}

static int pciep_info(int argc, char *argv[])
{
	int pf_idx, ep_idx = 0;
	struct pciep_dma_dev *pcidma;

	if (argc < 3) {
		fprintf(stderr,
			"info correct format:\n"
			"\t info [PF index][VF index]\n");
		return -EINVAL;
	}

	pf_idx = strtoul(argv[1], 0, 0);
	ep_idx = strtoul(argv[2], 0, 0);

	pcidma = pciep_find(pf_idx, ep_idx);
	if (!pcidma)
		return -EINVAL;

	vfio_pci_ep_info(pcidma->ep);

	return 0;
}

static int pciep_dump(int argc, char *argv[])
{
	int pf_idx, ep_idx = 0, err;
	struct pciep_dma_dev *pcidma;
	uint32_t offset = 0, length = 64, i;
	volatile uint32_t *p;

	if (argc < 4) {
		err = -EINVAL;
		goto _err;
	}

	pf_idx = strtoul(argv[1], 0, 0);
	ep_idx = strtoul(argv[2], 0, 0);

	pcidma = pciep_find(pf_idx, ep_idx);
	if (!pcidma) {
		fprintf(stderr,
			"pf index or vf index is wrong\n");
		return -EINVAL;
	}

	if (!strcmp(argv[3], "reg"))
		p = pcidma->pf->reg;
	else if (!strcmp(argv[3], "config"))
		p = (uint32_t *)pcidma->config;
	else if (!strcmp(argv[3], "local_buffer"))
		p = pcidma->local_buffer;
	else if (!strcmp(argv[3], "remote_buffer")) {
		if (!pcidma->worker ||
		    pcidma->worker->status != PCIDMA_STATUS_DONE) {
			fprintf(stderr,
				"remote point did not create remote buffer");
			p = NULL;
		} else
			p = pcidma->remote_buffer;
	} else {
		err = -EINVAL;
		goto _err;
	}

	if (!p)
		return 0;

	if (argc > 4)
		length = strtoul(argv[4], 0, 0);

	fprintf(stderr, "dump %s:\n", argv[3]);
	for (i = 0; i < length/4; i += 4) {
		fprintf(stderr, "%08x: 0x%08x 0x%08x 0x%08x 0x%08x\n",
			offset + i, p[i], p[i+1], p[i+2], p[i+3]);
	}

	return 0;

_err:
	fprintf(stderr,
		"dump correct format:\n"
		"\t dump [pf idx] [vf idx] "
		"[reg config local_buffer remote_buffer] "
		"[length]\n");
	return err;

}

static int pciep_add(int argc, char *argv[])
{
	int pf_idx, ep_idx = 0;
	struct pciep_dma_dev *pcidma;

	if (argc < 3) {
		fprintf(stderr,
			"add correct format:\n"
			"\t add [pf idx] [vf idx]\n");
		return -EINVAL;
	}

	pf_idx = strtoul(argv[1], 0, 0);
	ep_idx = strtoul(argv[2], 0, 0);

	pcidma = pciep_find(pf_idx, ep_idx);
	if (!pcidma) {
		fprintf(stderr, "pf idx or vf idx is wrong\n");
		return -EINVAL;
	}

	worker_new(pcidma);

	return 0;
}

static int pciep_rm(int argc, char *argv[])
{
	int pf_idx, ep_idx = 0;
	struct pciep_dma_dev *pcidma;

	if (argc < 3) {
		fprintf(stderr,
			"rm correct format:\n"
			"\t dump [pf idx] [vf idx]\n");
		return -EINVAL;
	}

	pf_idx = strtoul(argv[1], 0, 0);
	ep_idx = strtoul(argv[2], 0, 0);

	pcidma = pciep_find(pf_idx, ep_idx);
	if (!pcidma) {
		fprintf(stderr, "pf idx or vf idx is wrong\n");
		return -EINVAL;
	}

	worker_free(pcidma->worker);

	return 0;
}

static int pciep_list(int argc, char *argv[])
{
	struct worker *worker;
	struct pciep_dma_dev *pcidma;

	if (!host->pf)
		return 0;

	list_for_each_entry(worker, &workers, node) {
		pcidma = worker->pvt;
		fprintf(stderr, "%s\'s test thread is runing on cpu%d\n",
			pcidma->name, worker->cpu);
	}

	return 0;
}

static int pciep_msix(int argc, char *argv[])
{
	int pf_idx, ep_idx, msix;
	struct pciep_dma_dev *pcidma;

	if (argc < 4) {
		fprintf(stderr,
			"msix correct format:\n"
			"\t msix [pf idx] [vf idx] [msix entry]\n");
		return -EINVAL;
	}

	pf_idx = strtoul(argv[1], 0, 0);
	ep_idx = strtoul(argv[2], 0, 0);
	msix = strtoul(argv[3], 0, 0);

	pcidma = pciep_find(pf_idx, ep_idx);
	if (!pcidma) {
		fprintf(stderr, "pf idx or vf idx is wrong\n");
		return -EINVAL;
	}

	pcidma_send_msix(pcidma, msix);

	return 0;
}

cli_cmd(help, pciep_cli_help);
cli_cmd(info, pciep_info);
cli_cmd(dump, pciep_dump);
cli_cmd(add, pciep_add);
cli_cmd(rm, pciep_rm);
cli_cmd(list, pciep_list);
cli_cmd(msix, pciep_msix);

int main(int argc, char *argv[])
{
	int rt, cli_argc;
	int controller_idx = 0, worker_num = 0;
	char *cli, **cli_argv;
	const struct cli_table_entry *cli_cmd;
	struct pci_controller *controller = NULL;
	struct worker *worker, *tmpworker;
	int i;

	rt = of_init();
	if (rt) {
		error(0, 0, "of_init() failed\n");
		exit(EXIT_FAILURE);
	}

	ncpus = (unsigned long)sysconf(_SC_NPROCESSORS_ONLN);

	atb_multiplier = atb_get_multiplier();
	if (atb_multiplier < 0) {
		error(0, 0, "failed to get cpu clock\n");
		goto leave;
	}
	PCIEP_DBG("CPU clock is %lldHZ\n", atb_multiplier);

	/* - map DMA mem */
	dma_mem_generic =
			dma_mem_create(DMA_MAP_FLAG_ALLOC, NULL, DMA_MEM_SIZE);
	if (!dma_mem_generic) {
		error(0, 0, "failed to create dma memory\n");
		goto leave;
	}
	PCIEP_DBG("Allocated DMA region size 0x%zx\n", DMA_MEM_SIZE);

	if (argc > 1)
		controller_idx = strtoul(argv[1], 0, 0);

	if (argc > 2)
		worker_num = strtoul(argv[2], 0, 0);

	controller = malloc(sizeof(*controller));
	if (!controller) {
		error(0, 0, "failed to allocate memory for controller\n");
		goto leave;
	}

	host = controller;
	if (pci_controller_init(controller, controller_idx))
		goto leave;

	/* DMA channel init */
	for (i = 0; i < DMA_CH_USE; i++) {
		if (i >= FSL_DMA_CH_NUM) {
			error(0, 0, "set channel num %d > dma max channel %d\n",
				DMA_CH_USE, FSL_DMA_CH_NUM);
			error(0, 0, "now %d channels can be used\n", FSL_DMA_CH_NUM);
			ch_used = FSL_DMA_CH_NUM;
			break;
		}
		rt = fsl_dma_chan_init(&dmadevs[i], i / FSL_DMA_CH_CON, i % FSL_DMA_CH_CON);
		if (rt < 0) {
			error(0, -rt, "failed to initialize DMA channel %d\n", i);
			goto leave;
		}
		fsl_dma_chan_bwc(dmadevs[i], DMA_BWC_1024);
	}

	/* start workers for test */
	workers_auto_new(worker_num);

	/* Run the CLI loop */
	while (1) {
		/* Reap any dead threads */
		list_for_each_entry_safe(worker, tmpworker, &workers, node)
			if (!worker_reap(worker))
				error(0, 0,
				      "Caught dead thread on cpu %d",
				      worker->cpu);

		/* Get CLI input */
		cli = readline(pciep_prompt);
		if (unlikely((cli == NULL) || strncmp(cli, "q", 1) == 0))
			break;
		if (cli[0] == 0) {
			free(cli);
			continue;
		}

		cli_argv = history_tokenize(cli);
		if (unlikely(cli_argv == NULL)) {
			fprintf(stderr,
				"Out of memory while parsing: %s\n", cli);
			free(cli);
			continue;
		}

		for (cli_argc = 0; cli_argv[cli_argc] != NULL; cli_argc++)
			;

		foreach_cli_table_entry(cli_cmd) {
			if (strcmp(cli_argv[0], cli_cmd->cmd) == 0) {
				rt = cli_cmd->handle(cli_argc, cli_argv);
				if (unlikely(rt < 0))
					fprintf(stderr, "%s: %s\n",
						cli_cmd->cmd, strerror(-rt));
				add_history(cli);
				break;
			}
		}

		if (cli_cmd == cli_table_end)
			fprintf(stderr, "Unknown command: %s\n", cli);

		for (cli_argc = 0; cli_argv[cli_argc] != NULL; cli_argc++)
			free(cli_argv[cli_argc]);
		free(cli_argv);
		free(cli);
	}
	/* success */
	rt = 0;

leave:
	of_finish();

	for (i = 0; i < ch_used; i++)
		fsl_dma_chan_finish(dmadevs[i]);

	if (controller)
		pci_controller_free(controller);
	if (dma_mem_generic)
		dma_mem_destroy(dma_mem_generic);

	return rt;
}
