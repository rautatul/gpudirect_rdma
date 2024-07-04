/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 Attala Systems Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/queue.h>
#include <zlib.h>
#include <sys/time.h>
#include <sys/sysinfo.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include "hugepage.h"

#define NUM_SGE (1)
#define DEFAULT_QDEPTH	8
#define SERVER_PORT_BASE 11235

#define TEST_NZ(x)      do { \
				if ((x)) { \
					printf("error: %s returned !0.", #x); \
					die(); \
				} \
			} while (0)
#define TEST_Z(x)       do { \
				if (!(x)) { \
					printf("error: %s returned 0.", #x); \
					die(); \
				} \
			} while (0)
#ifdef DEBUG_ENABLE
	#define DEBUG_PRINT(args...) printf(args)
#else
	#define DEBUG_PRINT(args...)
#endif


#define MIN_QP 1
#define MAX_QP 2048
#define MIN_CQ 1
#define MAX_CQ 2048
#define MAX_COMP_CHANNELS 1000
#define MAX_PAUSE_CNT	  8

#define MIN_BLOCKSIZE 1
#define MAX_BLOCKSIZE 1048576
#define MIN_SGE 1
#define MAX_SGE 7
#define MIN_LOOPS 1
#define MAX_LOOPS 1000000

enum type {
	MSG_MR,
	MSG_DONE
};

enum mode {
	M_WRITE,
	M_READ,
	M_VERIFY
};

enum role {
	ROLE_CLIENT,
	ROLE_SERVER
};

enum error_handling {
	DIE_ON_ERROR,
	HANG_ON_ERROR
};

enum verify_op {
	VERIFY_PATTERN,
	VERIFY_RANDOM
};

enum verify_stage {
	VERIFY_STAGE1, // Init state
	VERIFY_STAGE2, // Initial Read Client -> Server
};

enum queue_mode {
	QUEUE_NORMAL,
	QUEUE_SHARED
};

enum page_mode {
	PAGE_DEFAULT, //4K
	PAGE_HUGE //2M
};

struct user_data {
	uint8_t rdma_op;
	uint8_t no_sge;
	uint16_t qdepth;
	int blocksize;
};

/* Hugepage */
extern struct hugepageallocator_t gAllocator;

void die(void);

void build_context(struct ibv_context *verbs);
void destroy_context(void);
void build_connection(struct rdma_cm_id *id);
void reg_mem_and_post_recv(struct rdma_cm_id *id);
void initialize_queues(unsigned int num_qps, unsigned int num_cqs);
void build_params(struct rdma_conn_param *params, void *user_data);
void destroy_connection(void *context);
void *get_local_message_region(void *context, int sge, enum mode m);
void on_connect(void *context);
void send_mr(void *context);
void complete_iteration(void *context);
void set_operating_mode(enum queue_mode q, enum page_mode p);
void set_user_data(struct user_data u_data);
void set_pattern(uint64_t pattern, enum verify_op v_op);
void set_test_params(uint64_t loop, enum role r, enum error_handling on_error);
void release_cq_lock(void);
void create_qp_handler(struct rdma_cm_id **conn_array);
void set_qdepth(uint16_t qdepth);
void start_bw_timer(void);
void stop_bw_timer(void);

#endif
