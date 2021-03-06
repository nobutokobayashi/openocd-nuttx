/***************************************************************************
 *   Copyright (C) 2016-2017 by Sony Corporation                           *
 *   Masatoshi Tateishi - Masatoshi.Tateishi@jp.sony.com                   *
 *   Masayuki Ishikawa - Masayuki.Ishikawa@jp.sony.com                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/jtag.h>
#include "target/target.h"
#include "target/target_type.h"
#include "target/armv7m.h"
#include "target/cortex_m.h"
#include "rtos.h"
#include "helper/log.h"
#include "helper/types.h"
#include "server/gdb_server.h"

#include "nuttx_header.h"


int rtos_thread_packet(struct connection *connection, const char *packet, int packet_size);

#ifdef CONFIG_DISABLE_SIGNALS
#define SIG_QUEUE_NUM 0
#else
#define SIG_QUEUE_NUM 1
#endif /* CONFIG_DISABLE_SIGNALS */

#ifdef CONFIG_DISABLE_MQUEUE
#define M_QUEUE_NUM 0
#else
#define M_QUEUE_NUM 2
#endif /* CONFIG_DISABLE_MQUEUE */

#ifdef CONFIG_PAGING
#define PAGING_QUEUE_NUM 1
#else
#define PAGING_QUEUE_NUM 0
#endif /* CONFIG_PAGING */


#define TASK_QUEUE_NUM (6 + SIG_QUEUE_NUM + M_QUEUE_NUM + PAGING_QUEUE_NUM)


/* see nuttx/sched/os_start.c */
static char *nuttx_symbol_list[] = {
	"g_readytorun",            /* 0: must be top of this array */
	"g_tasklisttable",
	NULL
};

/* see nuttx/include/nuttx/sched.h */
struct tcb_s {
	uint32_t flink;
	uint32_t blink;
	uint8_t  dat[256];
};

struct {
	uint32_t addr;
	uint32_t prio;
} g_tasklist[TASK_QUEUE_NUM];

static char *task_state_str[] = {
	"INVALID",
	"PENDING",
	"READYTORUN",
	"RUNNING",
	"INACTIVE",
	"WAIT_SEM",
#ifndef CONFIG_DISABLE_SIGNALS
	"WAIT_SIG",
#endif /* CONFIG_DISABLE_SIGNALS */
#ifndef CONFIG_DISABLE_MQUEUE
	"WAIT_MQNOTEMPTY",
	"WAIT_MQNOTFULL",
#endif /* CONFIG_DISABLE_MQUEUE */
#ifdef CONFIG_PAGING
	"WAIT_PAGEFILL",
#endif /* CONFIG_PAGING */
};

/* see arch/arm/include/armv7-m/irq_cmnvector.h */
static const struct stack_register_offset nuttx_stack_offsets_cortex_m[] = {
	{ 0x28, 32 },		/* r0   */
	{ 0x2c, 32 },		/* r1   */
	{ 0x30, 32 },		/* r2   */
	{ 0x34, 32 },		/* r3   */
	{ 0x08, 32 },		/* r4   */
	{ 0x0c, 32 },		/* r5   */
	{ 0x10, 32 },		/* r6   */
	{ 0x14, 32 },		/* r7   */
	{ 0x18, 32 },		/* r8   */
	{ 0x1c, 32 },		/* r9   */
	{ 0x20, 32 },		/* r10  */
	{ 0x24, 32 },		/* r11  */
	{ 0x38, 32 },		/* r12  */
	{   0,  32 },		/* sp   */
	{ 0x3c, 32 },		/* lr   */
	{ 0x40, 32 },		/* pc   */
	{ 0x44, 32 },		/* xPSR */
};


static const struct rtos_register_stacking nuttx_stacking_cortex_m = {
	0x48,                                   /* stack_registers_size */
	-1,                                     /* stack_growth_direction */
	17,                                     /* num_output_registers */
	0,                                      /* stack_alignment */
	nuttx_stack_offsets_cortex_m   /* register_offsets */
};

static uint8_t pid_offset = PID;
static uint8_t state_offset = STATE;
static uint8_t name_offset =  NAME;
static uint8_t xcpreg_offset = XCPREG;
static uint8_t name_size = NAME_SIZE;

static int rcmd_offset(const char *cmd, const char *name)
{
	if (strncmp(cmd, name, strlen(name)))
		return -1;

	if (strlen(cmd) <= strlen(name) + 1)
		return -1;

	return atoi(cmd + strlen(name));
}

static int nuttx_thread_packet(struct connection *connection,
	char const *packet, int packet_size)
{
	char cmd[GDB_BUFFER_SIZE / 2] = "";

	if (!strncmp(packet, "qRcmd", 5)) {
		int len = unhexify((uint8_t *)cmd, packet + 6, sizeof(cmd));
		int offset;

		if (len <= 0)
			goto pass;

		offset = rcmd_offset(cmd, "nuttx.pid_offset");

		if (offset >= 0) {
			LOG_INFO("pid_offset: %d", offset);
			pid_offset = offset;
			goto retok;
		}

		offset = rcmd_offset(cmd, "nuttx.state_offset");

		if (offset >= 0) {
			LOG_INFO("state_offset: %d", offset);
			state_offset = offset;
			goto retok;
		}

		offset = rcmd_offset(cmd, "nuttx.name_offset");

		if (offset >= 0) {
			LOG_INFO("name_offset: %d", offset);
			name_offset = offset;
			goto retok;
		}

		offset = rcmd_offset(cmd, "nuttx.xcpreg_offset");

		if (offset >= 0) {
			LOG_INFO("xcpreg_offset: %d", offset);
			xcpreg_offset = offset;
			goto retok;
		}

		offset = rcmd_offset(cmd, "nuttx.name_size");

		if (offset >= 0) {
			LOG_INFO("name_size: %d", offset);
			name_size = offset;
			goto retok;
		}

	}
pass:
	return rtos_thread_packet(connection, packet, packet_size);
retok:
	gdb_put_packet(connection, "OK", 2);
	return ERROR_OK;
}


static int nuttx_detect_rtos(struct target *target)
{
	if ((target->rtos->symbols != NULL) &&
			(target->rtos->symbols[0].address != 0) &&
			(target->rtos->symbols[1].address != 0)) {
		return 1;
	}
	return 0;
}

static int nuttx_create(struct target *target)
{

	target->rtos->gdb_thread_packet = nuttx_thread_packet;
	LOG_INFO("target type name = %s", target->type->name);
	return 0;
}

static int nuttx_update_threads(struct rtos *rtos)
{
	int thread_count;
	struct tcb_s tcb;
	int ret;
	uint32_t head;
	uint32_t tcb_addr;
	int i;

	thread_count = 0;

	/* free old thread info */
	if (rtos->thread_details) {
		for (i = 0; i < rtos->thread_count; i++) {
			if (rtos->thread_details[i].thread_name_str)
				free(rtos->thread_details[i].thread_name_str);
		}

		free(rtos->thread_details);
		rtos->thread_details = NULL;
		rtos->thread_count = 0;
	}

	ret = target_read_buffer(rtos->target, rtos->symbols[1].address,
		sizeof(g_tasklist), (uint8_t *)&g_tasklist);
	if (ret) {
		LOG_ERROR("target_read_buffer : ret = %d\n", ret);
		return ERROR_FAIL;
	}

	for (i = 0; i < (int)TASK_QUEUE_NUM; i++) {

		if (g_tasklist[i].addr == 0)
			continue;

		ret = target_read_u32(rtos->target, g_tasklist[i].addr,
			&head);

		if (ret) {
			LOG_ERROR("target_read_u32 : ret = %d\n", ret);
			return ERROR_FAIL;
		}

		/* readytorun head is current thread */
		if (g_tasklist[i].addr == rtos->symbols[0].address)
			rtos->current_thread = head;


		tcb_addr = head;
		while (tcb_addr) {
			struct thread_detail *thread;
			ret = target_read_buffer(rtos->target, tcb_addr,
				sizeof(tcb), (uint8_t *)&tcb);
			if (ret) {
				LOG_ERROR("target_read_buffer : ret = %d\n",
					ret);
				return ERROR_FAIL;
			}
			thread_count++;

			rtos->thread_details = realloc(rtos->thread_details,
				sizeof(struct thread_detail) * thread_count);
			thread = &rtos->thread_details[thread_count - 1];
			thread->threadid = tcb_addr;
			thread->exists = true;

			if (name_offset) {
				thread->thread_name_str = malloc(name_size + 1);
				snprintf(thread->thread_name_str, name_size,
				    "%s", (char *)&tcb.dat[name_offset - 8]);
			} else
				thread->thread_name_str = NULL;

			thread->extra_info_str =
			    task_state_str[tcb.dat[state_offset - 8]];
			tcb_addr = tcb.flink;
		}
	}
	rtos->thread_count = thread_count;

	return 0;
}


/*
 * thread_id = tcb address;
 */
static int nuttx_get_thread_reg_list(struct rtos *rtos, int64_t thread_id,
	char **hex_reg_list) {

	*hex_reg_list = NULL;

	return rtos_generic_stack_read(rtos->target, &nuttx_stacking_cortex_m,
	    (uint32_t)thread_id + xcpreg_offset, hex_reg_list);
};

static int nuttx_get_symbol_list_to_lookup(symbol_table_elem_t *symbol_list[])
{
	unsigned int i;

	*symbol_list = (symbol_table_elem_t *) calloc(1,
		sizeof(symbol_table_elem_t) * ARRAY_SIZE(nuttx_symbol_list));

	for (i = 0; i < ARRAY_SIZE(nuttx_symbol_list); i++)
		(*symbol_list)[i].symbol_name = nuttx_symbol_list[i];

	return 0;
}

struct rtos_type nuttx_rtos = {
	.name = "nuttx",
	.detect_rtos = nuttx_detect_rtos,
	.create = nuttx_create,
	.update_threads = nuttx_update_threads,
	.get_thread_reg_list = nuttx_get_thread_reg_list,
	.get_symbol_list_to_lookup = nuttx_get_symbol_list_to_lookup,
};

