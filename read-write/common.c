// SPDX-License-Identifier: GPL-2.0

#include "common.h"

#define DEBUG_PRINT_OFF  0
#define CRC_ENABLED	 1
#define EXTRA_RECVS	 0
#define STOP_ON_SLOW_QP  1
// Currently QP_LOCK_STEP works only for Qdepth=1
// And CQs <= 1000
#define QP_LOCK_STEP     1
#define container_of(ptr, type, member) ({                      \
	const typeof(((type *)0)->member) * __mptr = (ptr);     \
	(type *)((char *)__mptr - offsetof(type, member)); })

/* Structures */
struct message {
	uint32_t seq_num;
	uint32_t seq_id;
	uint16_t msg_type;
	uint16_t msg_op;
	struct {
		struct ibv_mr mr[2];
	} data;
	uint32_t crc32;
};

struct send_msg {
	TAILQ_ENTRY(send_msg) next;
	struct message buffer;
	struct ibv_mr *send_local_mr;
	struct ibv_send_wr wr;
	struct ibv_sge sge;
	struct connection *conn;
};

struct recv_msg {
	TAILQ_ENTRY(recv_msg) next;
struct message buffer;
	struct ibv_mr *send_local_mr;
	struct ibv_recv_wr wr;
	struct ibv_sge sge;
	struct connection *conn;
};

struct rdma_region {
	TAILQ_ENTRY(rdma_region) next;
	uint32_t seq_num;
	uint32_t seq_id;
	char *data_buffer;
	struct ibv_mr *data_mr[2];
	enum verify_stage v_stage;
	struct connection *conn;
	struct ibv_mr peer_mr[2];
	struct ibv_send_wr wr;
	struct ibv_sge sge[MAX_SGE];
};

struct context {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq **cq_array;
	struct ibv_srq *srq;
	struct ibv_comp_channel **comp_channel_array;
	pthread_t *cq_poller_thread_array;
	pthread_t qp_handler_thread;
	int send_archived;

	pthread_spinlock_t cq_lock;
	pthread_spinlock_t global_lock;
};

struct connection {
	struct rdma_cm_id *id;
	struct ibv_qp *qp;
	struct ibv_srq *srq;

	TAILQ_HEAD(, send_msg) send_posted_head;
	TAILQ_HEAD(, recv_msg) recv_posted_head;
	TAILQ_HEAD(, rdma_region) rdma_posted_head;
	TAILQ_HEAD(, send_msg) send_free_head;
	TAILQ_HEAD(, recv_msg) recv_free_head;
	TAILQ_HEAD(, rdma_region) rdma_free_head;

	int connected;
	int send_msg_count;
	int recv_msg_count;
	int rdma_reg_count;
	int send_done;
	int recv_done;
	int loop_count;
	int pause_cnt;
	int last_min;

	pthread_spinlock_t send_free_lock;
	pthread_spinlock_t rdma_free_lock;
};

/* Declarations */
static void build_srq_attr(struct ibv_srq_init_attr *srq_attr);
static void build_qp_attr(struct ibv_qp_init_attr *qp_attr);
static void on_completion(struct ibv_wc *);
static void *poll_qps(void *);
static void *poll_cq(void *);
static void post_single_receive(struct connection *conn);
static void post_receives(struct connection *conn);
static void register_memory(struct connection *conn);
static void do_rdma_op(void *context, struct rdma_region **rdma_ptr);
static void send_message(struct connection *conn,
			 struct send_msg **send_msg_ptr);
//static void generate_sge_distribution();
static void populate_sge(struct rdma_region *temp_rdma);
//static char *get_message_region(struct rdma_region *rdma_ptr, int sge);
static void fill_pattern(struct rdma_region **rdma_ptr);
static void compare_data_buffer(struct rdma_region **rdma_ptr);
static void print_bandwidth(void);

/* Globals */
static enum queue_mode s_queue_mode = QUEUE_NORMAL;
static enum page_mode s_page_mode = PAGE_DEFAULT;
static int s_cpu_count = 1;
static unsigned int s_num_qps = 1;
static unsigned int s_num_cqs = 1;
static unsigned int s_num_channels = 1;
static struct context *s_ctx;
static struct rdma_cm_id **s_conn_array;
static enum mode s_mode = M_WRITE;
static enum role s_role = ROLE_CLIENT;
static int s_rdma_buffer_size = 2048;
static uint16_t s_qdepth = 1;
static uint8_t s_sge = 1;
static unsigned int s_loops = MIN_LOOPS;
static unsigned int s_qp_index;
static unsigned int s_qp_count;
//static int *s_sge_distribution; //array the length of s_sge
static enum error_handling s_on_error = DIE_ON_ERROR;
static enum verify_op s_verify_mode = VERIFY_PATTERN;
static uint64_t s_verify_pattern = 0x12345678;
#if EXTRA_RECVS
static int MIN_POST_RECV = 64;
#endif

/* Timer variables */
static struct timeval s_start, s_stop;
static double s_bandwidth_time;
static double s_bandwidth;

/* Definitions */
void die(void)
{
	if (s_on_error == HANG_ON_ERROR) {
		// Cancelling the constant qp polling
		if (s_role == ROLE_CLIENT)
			pthread_cancel(s_ctx->qp_handler_thread);

		fprintf(stderr, "Hanging Indefinitely...\n");
		while (1)
			sleep(1000);
	}
	fprintf(stderr, "Dying Instantly...\n");

	exit(EXIT_FAILURE);
}

void build_connection(struct rdma_cm_id *id)
{
	struct connection *conn;
	struct ibv_qp_init_attr qp_attr;

	build_qp_attr(&qp_attr);
	TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));
	DEBUG_PRINT("id->qp_num: 0x%x | id->verbs address: %p\n",
		     id->qp->qp_num, id->verbs);

	id->context = (struct connection *)malloc(sizeof(struct connection));
	conn = id->context;
	conn->id = id;
	conn->qp = id->qp;
	if (s_queue_mode == QUEUE_SHARED)
		conn->srq = s_ctx->srq;

	TAILQ_INIT(&conn->send_free_head);
	TAILQ_INIT(&conn->recv_free_head);
	TAILQ_INIT(&conn->rdma_free_head);
	TAILQ_INIT(&conn->send_posted_head);
	TAILQ_INIT(&conn->recv_posted_head);
	TAILQ_INIT(&conn->rdma_posted_head);
	conn->loop_count = 0;
	conn->recv_msg_count = 0;
	conn->send_msg_count = 0;
	conn->rdma_reg_count = 0;
	conn->send_done = 0;
	conn->recv_done = 0;
	conn->pause_cnt = 0;
	conn->connected = 0;

	TEST_NZ(pthread_spin_init(&conn->send_free_lock,
				  PTHREAD_PROCESS_SHARED));
	TEST_NZ(pthread_spin_init(&conn->rdma_free_lock,
				  PTHREAD_PROCESS_SHARED));
	s_qp_index += 1;
}

void reg_mem_and_post_recv(struct rdma_cm_id *id)
{
	register_memory(id->context);
	post_receives(id->context);
	//generate_sge_distribution();
}

void initialize_queues(unsigned int num_qps, unsigned int num_cqs)
{
	s_num_qps = num_qps;
	s_num_cqs = num_cqs;
	s_ctx = NULL;

	if (s_num_cqs > s_num_qps)
		s_num_cqs = s_num_qps;

	s_num_channels = s_num_cqs > MAX_COMP_CHANNELS ?
			 MAX_COMP_CHANNELS : s_num_cqs;
	s_ctx = (struct context *)malloc(sizeof(struct context));
	s_ctx->cq_array = malloc(sizeof(struct ibv_cq *) * s_num_cqs);
	s_ctx->comp_channel_array =
		malloc(sizeof(struct ibv_comp_channel *) * s_num_channels);
	s_ctx->cq_poller_thread_array =
		malloc(sizeof(pthread_t) * s_num_channels);
}

/*
 * NOTE: Run build_context with verbs only once
 * as verbs is tied to the same open device fd
 */
void build_context(struct ibv_context *verbs)
{
	struct ibv_srq_init_attr srq_attr = {0};
	int comp_index, qps_per_cq, i;

#if QP_LOCK_STEP
	if (s_num_qps != s_num_cqs) {
		printf("%s: QP_LOCK_STEP needs num_qps = num_cqs\n", __func__);
		die();
	}
#endif
	s_conn_array = NULL;
	s_bandwidth_time = 0;
	s_bandwidth = 0;
	s_qp_index = 0;
	s_qp_count = 0;
	s_ctx->send_archived = 0;
	s_ctx->ctx = verbs;
	/* Create PD and associate SRQ if
	 * if mode is requested
	 */
	TEST_Z(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx));
	qps_per_cq = (s_num_qps / s_num_cqs) + (s_num_qps % s_num_cqs) ? 1 : 0;

	if (s_queue_mode == QUEUE_SHARED) {
		build_srq_attr(&srq_attr);
		s_ctx->srq = ibv_create_srq(s_ctx->pd, &srq_attr);
		if (s_ctx->srq == NULL) {
			printf("%s: Failed to create SRQ\n", __func__);
			die();
		}
	}

	TEST_NZ(pthread_spin_init(&s_ctx->global_lock, PTHREAD_PROCESS_SHARED));
	TEST_NZ(pthread_spin_init(&s_ctx->cq_lock, PTHREAD_PROCESS_SHARED));
	TEST_NZ(pthread_spin_lock(&s_ctx->cq_lock));

	// Create completion channels not more than 1000
	for (i = 0; i < s_num_channels; i++) {
		TEST_Z(s_ctx->comp_channel_array[i] =
			ibv_create_comp_channel(s_ctx->ctx));
		DEBUG_PRINT("%s: comp_channel(%d) address: %p\n", __func__,
			     i, &s_ctx->comp_channel_array[i]);
	}

	// Create CQs and assign the created channels to them
	for (i = 0; i < s_num_cqs; i++) {
		comp_index = i % s_num_channels;
		TEST_Z(s_ctx->cq_array[i] = ibv_create_cq(s_ctx->ctx,
				    ((s_qdepth + 1) * 4 * qps_per_cq),
				    NULL,
				    s_ctx->comp_channel_array[comp_index],
				    i % get_nprocs_conf()));
		TEST_NZ(ibv_req_notify_cq(s_ctx->cq_array[i], 0));
	}

	// Create a separate thread for each completion channel
	for (i = 0; i < s_num_channels; i++)
		TEST_NZ(pthread_create(&s_ctx->cq_poller_thread_array[i], NULL,
					poll_cq, (void *)(uintptr_t)i));
}

void destroy_context(void)
{
	int i;

	for (i = 0; i < s_num_channels; i++)
		pthread_cancel(s_ctx->cq_poller_thread_array[i]);

	for (i = 0; i < s_num_cqs; i++)
		ibv_destroy_cq(s_ctx->cq_array[i]);

	for (i = 0; i < s_num_channels; i++)
		ibv_destroy_comp_channel(s_ctx->comp_channel_array[i]);

	pthread_spin_destroy(&s_ctx->cq_lock);
	pthread_spin_destroy(&s_ctx->global_lock);

	free(s_ctx->comp_channel_array);
	free(s_ctx->cq_array);
	free(s_ctx->cq_poller_thread_array);
	free(s_ctx);
}

void build_params(struct rdma_conn_param *params, void *u_data)
{
	memset(params, 0, sizeof(*params));
	params->private_data = u_data;
	params->private_data_len = sizeof(struct user_data);
	params->initiator_depth = params->responder_resources = 1;
	params->retry_count = 3;
	params->rnr_retry_count = 3; /* 7 is infinite retry */

	if (s_page_mode == PAGE_HUGE)
		gAllocator.init();
}

void build_srq_attr(struct ibv_srq_init_attr *srq_attr)
{
	srq_attr->attr.max_wr = s_qdepth * s_num_qps;
	srq_attr->attr.max_sge = s_sge;
}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr)
{
	memset(qp_attr, 0, sizeof(*qp_attr));
	DEBUG_PRINT("%s: QP Index %d, CQ_index: %d\n",
		     __func__, s_qp_index, s_qp_index % s_num_cqs);
	qp_attr->send_cq = s_ctx->cq_array[s_qp_index % s_num_cqs];
	qp_attr->recv_cq = s_ctx->cq_array[s_qp_index % s_num_cqs];
	if (s_queue_mode == QUEUE_SHARED)
		qp_attr->srq = s_ctx->srq;

	qp_attr->qp_type = IBV_QPT_RC;
	qp_attr->cap.max_send_wr = s_qdepth + 1;
	qp_attr->cap.max_recv_wr = s_qdepth + 1;
	qp_attr->cap.max_send_sge = s_sge;
	qp_attr->cap.max_recv_sge = 1;
}

void destroy_connection(void *context)
{
	struct connection *conn = (struct connection *)context;
	struct send_msg *temp_send;
	struct recv_msg *temp_recv;
	struct rdma_region *temp_rdma;

	// Cancelling the constant qp polling
	if (s_role == ROLE_CLIENT)
		pthread_cancel(s_ctx->qp_handler_thread);

	rdma_destroy_qp(conn->id);

	temp_send = TAILQ_FIRST(&conn->send_posted_head);
	while (temp_send != NULL) {
		ibv_dereg_mr(temp_send->send_local_mr);
		TAILQ_REMOVE(&conn->send_posted_head, temp_send, next);
		free(temp_send);
		temp_send = TAILQ_FIRST(&conn->send_posted_head);
	}

	temp_send = TAILQ_FIRST(&conn->send_free_head);
	while (temp_send != NULL) {
		ibv_dereg_mr(temp_send->send_local_mr);
		TAILQ_REMOVE(&conn->send_free_head, temp_send, next);
		free(temp_send);
		temp_send = TAILQ_FIRST(&conn->send_free_head);
	}

	temp_recv = TAILQ_FIRST(&conn->recv_posted_head);
	while (temp_recv != NULL) {
		ibv_dereg_mr(temp_recv->send_local_mr);
		TAILQ_REMOVE(&conn->recv_posted_head, temp_recv, next);
		free(temp_recv);
		temp_recv = TAILQ_FIRST(&conn->recv_posted_head);
	}
	temp_recv = TAILQ_FIRST(&conn->recv_free_head);
	while (temp_recv != NULL) {
		ibv_dereg_mr(temp_recv->send_local_mr);
		TAILQ_REMOVE(&conn->recv_free_head, temp_recv, next);
		free(temp_recv);
		temp_recv = TAILQ_FIRST(&conn->recv_free_head);
	}

	temp_rdma = TAILQ_FIRST(&conn->rdma_posted_head);
	while (temp_rdma != NULL) {
		if (s_mode == M_VERIFY)
			ibv_dereg_mr(temp_rdma->data_mr[1]);
		ibv_dereg_mr(temp_rdma->data_mr[0]);
		TAILQ_REMOVE(&conn->rdma_posted_head, temp_rdma, next);
		if (s_page_mode == PAGE_HUGE)
			gAllocator.free(temp_rdma->data_buffer,
				s_rdma_buffer_size *
				(s_mode == M_VERIFY ? 2 : 1));
		else
			free(temp_rdma->data_buffer);
		free(temp_rdma);
		temp_rdma = TAILQ_FIRST(&conn->rdma_posted_head);
	}

	temp_rdma = TAILQ_FIRST(&conn->rdma_free_head);
	while (temp_rdma != NULL) {
		if (s_mode == M_VERIFY)
			ibv_dereg_mr(temp_rdma->data_mr[1]);
		ibv_dereg_mr(temp_rdma->data_mr[0]);
		TAILQ_REMOVE(&conn->rdma_free_head, temp_rdma, next);
		if (s_page_mode == PAGE_HUGE)
			gAllocator.free(temp_rdma->data_buffer,
				s_rdma_buffer_size *
				(s_mode == M_VERIFY ? 2 : 1));
		else
			free(temp_rdma->data_buffer);
		free(temp_rdma);
		temp_rdma = TAILQ_FIRST(&conn->rdma_free_head);
	}

	pthread_spin_destroy(&conn->send_free_lock);
	pthread_spin_destroy(&conn->rdma_free_lock);
	rdma_destroy_id(conn->id);
	free(conn);
}

/*
 * char *get_message_region(struct rdma_region *rdma_ptr, int sge)
 * {
 *	 return rdma_ptr->data_buffer + sge * s_sge_distribution[sge];
 * }
 */

void fill_pattern(struct rdma_region **rdma_ptr)
{
	struct rdma_region *temp_rdma = *rdma_ptr;
	unsigned char random_byte;
	int offset, size_left = s_rdma_buffer_size;

	memset(temp_rdma->data_buffer, 0, s_rdma_buffer_size * 2);
	if (s_verify_mode == VERIFY_PATTERN) {
		for (offset = 0; offset < s_rdma_buffer_size;
		     offset += 8, size_left -= 8) {
			memcpy(temp_rdma->data_buffer + offset,
			       &s_verify_pattern,
			       (size_left > 8) ? 8 : size_left);
		}
	} else {
		for (offset = 0; offset < s_rdma_buffer_size; offset += 1) {
			random_byte = rand();
			temp_rdma->data_buffer[offset] = random_byte;
		}
	}
}

void compare_data_buffer(struct rdma_region **rdma_ptr)
{
	struct rdma_region *temp_rdma = NULL;
	char *buffer_ptr = NULL;
	uint32_t start, offset;

	if (rdma_ptr == NULL) {
		printf("NULL Ptr: rdma_ptr\n");
		return;
	}

	temp_rdma = *rdma_ptr;
	if (temp_rdma->data_buffer == NULL) {
		printf("NULL Ptr: data_buffer\n");
		return;
	}

	buffer_ptr = temp_rdma->data_buffer;
	for (start = 0; start < s_rdma_buffer_size; start++) {
		offset = start + s_rdma_buffer_size;
		DEBUG_PRINT("start: %d | offset: %d\n", start, offset);
		if (buffer_ptr[start] != buffer_ptr[offset]) {
			printf("%s: Mismatch\n", __func__);
			printf("Mismatch at 0x%lx and 0x%lx\n",
				(uintptr_t)&buffer_ptr[start],
				(uintptr_t)&buffer_ptr[offset]);
			printf("Mismatch at 0x%x and 0x%x\n",
				buffer_ptr[start],
				buffer_ptr[offset]);
		}
	}
}

void on_completion(struct ibv_wc *wc)
{
	struct connection *conn;
	struct ibv_send_wr *send_wr;
	struct ibv_send_wr *rdma_wr;
	struct ibv_recv_wr *recv_wr;
	struct send_msg *temp_send;
	struct recv_msg *temp_recv;
	struct rdma_region *temp_rdma;
	int i = 0;

	DEBUG_PRINT("%s: wc->status: %d | wc->opcode: %d\n",
		__func__, wc->status, wc->opcode);

	if (wc->status != IBV_WC_SUCCESS) {
		printf("%s: status is not IBV_WC_SUCCESS.\n", __func__);
		die();
	}

	//Increment counts to track if qdepth number of nodes
	//have been accounted for
	if (wc->opcode == IBV_WC_SEND) {
		send_wr = (struct ibv_send_wr *)wc->wr_id;
		temp_send = container_of(send_wr, struct send_msg, wr);
		conn = temp_send->conn;
		conn->send_done++;

		TEST_NZ(pthread_spin_lock(&conn->send_free_lock));
		TAILQ_REMOVE(&conn->send_posted_head, temp_send, next);
		TAILQ_INSERT_TAIL(&conn->send_free_head, temp_send, next);
		TEST_NZ(pthread_spin_unlock(&conn->send_free_lock));

		if (s_role == ROLE_CLIENT) {
			if (temp_send->buffer.seq_id == s_qdepth - 1 &&
			    conn->recv_done == s_qdepth &&
			    conn->loop_count < s_loops) {
				DEBUG_PRINT("Sending MR for QP|Seq = 0x%x|%d\n",
					conn->qp->qp_num,
					conn->send_msg_count + 1);
				conn->send_done = 0;
				conn->recv_done = 0;
				complete_iteration(conn);
			}
		}
		return;
	}

	if (wc->opcode & IBV_WC_RECV) {
		recv_wr = (struct ibv_recv_wr *)wc->wr_id;
		temp_recv = container_of(recv_wr, struct recv_msg, wr);
		conn = temp_recv->conn;
		conn->recv_msg_count++;
		conn->recv_done++;

		TEST_NZ(pthread_spin_lock(&conn->rdma_free_lock));
		temp_rdma = TAILQ_FIRST(&conn->rdma_free_head);
		if (!temp_rdma) {
			printf("%s: No node in rdma_free queue\n", __func__);
			die();
		}
		TAILQ_REMOVE(&conn->rdma_free_head, temp_rdma, next);
		TEST_NZ(pthread_spin_unlock(&conn->rdma_free_lock));

		TAILQ_REMOVE(&conn->recv_posted_head, temp_recv, next);
#if CRC_ENABLED
		uint32_t prev_crc = 0, current_crc = 0;

		prev_crc = temp_recv->buffer.crc32;
		temp_recv->buffer.crc32 = 0;
		current_crc = crc32(0, (uint8_t *)&temp_recv->buffer,
				    sizeof(struct message));
		if (prev_crc != current_crc) {
			printf("%s: CRC check failed\n", __func__);
			die();
		}
#endif

		if (s_role == ROLE_SERVER &&
		    temp_recv->buffer.msg_type == MSG_MR) {

			for (i = 0; i < (s_mode == M_VERIFY ? 2 : 1); i++)
				memcpy(&temp_rdma->peer_mr[i],
				       &temp_recv->buffer.data.mr[i],
				       sizeof(struct ibv_mr));

			temp_rdma->seq_num = temp_recv->buffer.seq_num;

			DEBUG_PRINT("Recv msg. QP|Seq num = 0x%x|%d\n",
				conn->qp->qp_num,
				temp_recv->buffer.seq_num);
			DEBUG_PRINT("temp_recv seq_id: %d\n",
				     temp_recv->buffer.seq_id);
			DEBUG_PRINT("temp_rdma seq_id: %d\n",
				     temp_rdma->seq_id);

			TAILQ_INSERT_TAIL(&conn->recv_free_head,
					  temp_recv, next);
			post_single_receive(conn);

			// DO READ/WRITE operations based on buffer.type
			do_rdma_op(conn, &temp_rdma);
			TAILQ_INSERT_TAIL(&conn->rdma_posted_head,
					  temp_rdma, next);

		} else if (s_role == ROLE_CLIENT &&
			   temp_recv->buffer.msg_type == MSG_DONE) {
			conn->rdma_reg_count++;
			DEBUG_PRINT("Transaction done. QP|Seq num = 0x%x|%d\n",
				conn->qp->qp_num, temp_recv->buffer.seq_num);

			TAILQ_INSERT_TAIL(&conn->recv_free_head,
					  temp_recv, next);
			post_single_receive(conn);

			// Wait till s_qdepth trans are done and then disconnect
			DEBUG_PRINT("temp_rdma Seq ID: %d\n",
				     temp_rdma->seq_id);
			if (s_mode == M_VERIFY) {
				if (temp_rdma != NULL) {
					DEBUG_PRINT("Comparing Buffers\n");
					compare_data_buffer(&temp_rdma);
				}
			}
			// Move the node to the back of the list
			TEST_NZ(pthread_spin_lock(&conn->rdma_free_lock));
			TAILQ_INSERT_TAIL(&conn->rdma_free_head,
					  temp_rdma, next);
			TEST_NZ(pthread_spin_unlock(&conn->rdma_free_lock));

			/* End of an Iteration check */
			if (temp_rdma->seq_id == s_qdepth - 1) {
				DEBUG_PRINT("%s: Entire list serviced\n",
					     __func__);
				conn->loop_count++;
				DEBUG_PRINT("QP: 0x%x | Loop Count: %d\n",
					     conn->qp->qp_num,
					     conn->loop_count);
			} //list_length check

			// Check if there are more iterations to do
			if (conn->loop_count < s_loops &&
			    temp_rdma->seq_id == s_qdepth - 1 &&
			    conn->send_done == s_qdepth) {
				DEBUG_PRINT("QP: 0x%x | Loop Count: %d\n",
					     conn->qp->qp_num,
					     conn->loop_count);
				DEBUG_PRINT("Sending MR for QP|Seq = 0x%x|%d\n",
					conn->qp->qp_num,
					conn->send_msg_count + 1);
				conn->send_done = 0;
				conn->recv_done = 0;
				complete_iteration(conn);
			}
		} else { //End of ROLE_CLIENT
			printf("%s: Bad RECV message\n", __func__);
			die();
		}
	} // End of IBV_WC_RECV

	if (wc->opcode == IBV_WC_RDMA_WRITE || wc->opcode == IBV_WC_RDMA_READ) {
		rdma_wr = (struct ibv_send_wr *)wc->wr_id;
		temp_rdma = container_of(rdma_wr, struct rdma_region, wr);
		conn = temp_rdma->conn;
		conn->rdma_reg_count++;
		DEBUG_PRINT("Reg Count: %d\n", conn->rdma_reg_count);
		TAILQ_REMOVE(&conn->rdma_posted_head, temp_rdma, next);

		if (s_mode == M_VERIFY && wc->opcode == IBV_WC_RDMA_READ) {
			DEBUG_PRINT("Read completed. QP|Seq num = 0x%x|%d\n",
				(conn->qp->qp_num),
				temp_rdma->seq_num);
			temp_rdma->v_stage = VERIFY_STAGE2;
			do_rdma_op(conn, &temp_rdma);
			TAILQ_INSERT_TAIL(&conn->rdma_posted_head,
					  temp_rdma, next);

		} else {
			if (s_mode == M_VERIFY &&
			    wc->opcode == IBV_WC_RDMA_WRITE) {
				DEBUG_PRINT("Write completed. "
					    "QP|Seq num = 0x%x|%d\n",
					    (conn->qp->qp_num),
					    temp_rdma->seq_num);
				temp_rdma->v_stage = VERIFY_STAGE1;
			} else {
				DEBUG_PRINT("RDMA completed. "
					    "QP|Seq num = 0x%x|%d\n",
					    (conn->qp->qp_num),
					    temp_rdma->seq_num);
			}

			// Send MSG_DONE using send_msg
			temp_send = TAILQ_FIRST(&conn->send_free_head);
			TAILQ_REMOVE(&conn->send_free_head, temp_send, next);
			temp_send->buffer.msg_type = MSG_DONE;
			temp_send->buffer.seq_num = temp_rdma->seq_num;

			TEST_NZ(pthread_spin_lock(&conn->rdma_free_lock));
			TAILQ_INSERT_TAIL(&conn->rdma_free_head,
					  temp_rdma, next);
			TEST_NZ(pthread_spin_unlock(&conn->rdma_free_lock));

			send_message(conn, &temp_send);
			conn->send_msg_count++;
			TAILQ_INSERT_TAIL(&conn->send_posted_head,
					  temp_send, next);

			/* Check if the last node was sent and if it was
			 * the last node, increment the loop_count
			 */
			if (temp_send->buffer.seq_id == s_qdepth - 1)
				conn->loop_count++;
		}
	} // End of WC_RDMA_WRITE || WC_RDMA_READ
        DEBUG_PRINT("client Ends - ATUL-8\n");
	DEBUG_PRINT("Loop Count: %d\n", conn->loop_count);
	DEBUG_PRINT("Send Count: %d\n", conn->send_msg_count);
	DEBUG_PRINT("Recv Count: %d\n", conn->recv_msg_count);

	TEST_NZ(pthread_spin_lock(&s_ctx->global_lock));
	if (conn->send_msg_count == ((s_qdepth * s_loops)) &&
		conn->recv_msg_count == ((s_qdepth * s_loops))) {
		s_qp_count += 1;
		DEBUG_PRINT("%s: QPs COMPLETED: %d\n", __func__, s_qp_count);
		if (s_role == ROLE_CLIENT && s_qp_count == s_num_qps) {
			print_bandwidth();
			printf("%s: COMPLETED TRANSACTIONS\n", __func__);
			for (i = 0; i < s_num_qps; i++)
				rdma_disconnect(s_conn_array[i]);
		}
	}
	TEST_NZ(pthread_spin_unlock(&s_ctx->global_lock));
}

void print_bandwidth(void)
{
	printf("%s: All QPs COMPLETED\n", __func__);
	stop_bw_timer();
	s_bandwidth += s_rdma_buffer_size * s_qdepth *
		       s_loops * s_num_qps /
		       (1024.0 * 1024.0);
	s_bandwidth_time = (s_stop.tv_sec - s_start.tv_sec) +
			   (s_stop.tv_usec - s_start.tv_usec) / 1000000.0;
	printf("Time elapsed: %lf s\n", s_bandwidth_time);
	printf("Bandwidth: %lf Mb/s\n", (s_bandwidth/s_bandwidth_time));
}
 
void on_connect(void *context)
{
	start_bw_timer();
	((struct connection *)context)->connected = 1;
}

void *poll_qps(void *conn_array)
{
	struct rdma_cm_id **id_array = (struct rdma_cm_id **)conn_array;
	struct connection *conn = NULL;
	struct connection *last_conn = NULL;
	struct ibv_wc wc[4];
	uint32_t min;
	int count = 0;
	int i = 0;

	// Allowing 1 sec for initiation
	sleep(1);

	while (1) {
		min = 0xffffffff;

		for (i = 0; i < s_num_qps; i++) {
			conn = (struct connection *)id_array[i]->context;

			if (conn->recv_msg_count < min) {
				min = conn->recv_msg_count;
				last_conn = conn;
			}
		}

		for (i = 0; i < s_num_qps; i++) {
			conn = (struct connection *)id_array[i]->context;
			if (conn != last_conn)
				conn->pause_cnt = 0;
		}

		if (last_conn->last_min == min)
			last_conn->pause_cnt++;
		else
			last_conn->pause_cnt = 0;

		if (last_conn->pause_cnt > MAX_PAUSE_CNT) {
#if 0
			printf("%s: DETECTED STUCK QP, COUNTs FOR ALL QPs:\n",
				__func__);
			for (i = 0; i < s_num_qps; i++) {
				conn =
				    (struct connection *)id_array[i]->context;
				count =
				    ibv_poll_cq(conn->qp->recv_cq, -99, &wc[0]);
				printf("%s: count for QP, QP_ID=0x%x CNT=0x%x "
				       "CQ_ID=0x%x LAST_HEAD_OFFSET=0x%x(%d)\n",
					__func__, conn->qp->qp_num,
					conn->recv_msg_count,
					count >> 16, (count & 0xFFFF) * 32,
					(count & 0xFFFF));
			}
#endif

			printf("%s: DEBUG QP STUCK, QP_ID=0x%x CNT=%d\n",
				__func__, last_conn->qp->qp_num,
				last_conn->last_min);
#if STOP_ON_SLOW_QP
			ibv_destroy_qp(last_conn->qp);
			count = ibv_poll_cq(last_conn->qp->recv_cq, 4, &wc[0]);
			printf("%s: DEBUG CQ Count= %d\n", __func__, count);
			printf("%s: DEBUG wc->status: %d | wc->opcode: %d\n",
				__func__, wc[0].status, wc[0].opcode);
			die();
#endif
		}
		printf("%s: POLLING IS ON. QP|CNT = 0x%x|%d\n",
			__func__, last_conn->qp->qp_num, min);
		last_conn->last_min = min;
		sleep(1);
	}

	return NULL;
}

void *poll_cq(void *ctx)
{
	struct ibv_cq *cq;
	struct ibv_wc wc;
	struct ibv_comp_channel *temp_channel;
	void *cq_ctx;
	int comp_channel_index = (int)(uintptr_t)ctx;

	temp_channel = s_ctx->comp_channel_array[comp_channel_index];
	DEBUG_PRINT("%s: called for comp_channel(%p) Index: %d\n",
		     __func__, temp_channel, comp_channel_index);

	// This is a checkpoint to make sure that all the init Send MRs
	// has been done on all QPs at this point
	TEST_NZ(pthread_spin_lock(&s_ctx->cq_lock));
	TEST_NZ(pthread_spin_unlock(&s_ctx->cq_lock));

	while (1) {
		// check comp channel for event- Block until event is received
		TEST_NZ(ibv_get_cq_event(temp_channel, &cq, &cq_ctx));
		// ack event
		ibv_ack_cq_events(cq, 1);
		TEST_NZ(ibv_req_notify_cq(cq, 0));

		while (ibv_poll_cq(cq, 1, &wc))
			on_completion(&wc);
	}

	return NULL;
}

void post_single_receive(struct connection *conn)
{
	struct ibv_recv_wr *bad_wr = NULL;
	struct recv_msg *temp_recv;

	temp_recv = TAILQ_FIRST(&conn->recv_free_head);
	if (temp_recv == NULL) {
		printf("%s: No nodes in recv_free head\n", __func__);
		die();
	}
	temp_recv->wr.wr_id = (uintptr_t)&temp_recv->wr;
	temp_recv->wr.next = NULL;
	temp_recv->conn = conn; //move this to register memory?
	temp_recv->wr.sg_list = &temp_recv->sge;
	temp_recv->wr.num_sge = 1;
	temp_recv->sge.addr = (uintptr_t)&temp_recv->buffer;
	temp_recv->sge.length = sizeof(struct message);
	temp_recv->sge.lkey = temp_recv->send_local_mr->lkey;

	if (s_queue_mode == QUEUE_NORMAL)
		TEST_NZ(ibv_post_recv(conn->qp, &temp_recv->wr, &bad_wr));
	else {
		DEBUG_PRINT("Posting Receive on shared receive queue\n");
		TEST_NZ(ibv_post_srq_recv(conn->srq, &temp_recv->wr, &bad_wr));
	}

	TAILQ_REMOVE(&conn->recv_free_head, temp_recv, next);
	TAILQ_INSERT_TAIL(&conn->recv_posted_head, temp_recv, next);
}

void post_receives(struct connection *conn)
{
	struct recv_msg *temp_recv = NULL;

	temp_recv = TAILQ_FIRST(&conn->recv_free_head);
	while (temp_recv != NULL) {
		post_single_receive(conn);
		temp_recv = TAILQ_FIRST(&conn->recv_free_head);
	}
}

void register_memory(struct connection *conn)
{
        void *gpuBuff = NULL;
	struct send_msg *temp_send = NULL;
	struct recv_msg *temp_recv = NULL;
	struct rdma_region *temp_rdma = NULL;
	int i, j;

	for (i = 0; i < s_qdepth; i++) {
		posix_memalign((void **)&temp_send, 4096,
				sizeof(struct send_msg));
		posix_memalign((void **)&temp_recv, 4096,
				sizeof(struct recv_msg));
		posix_memalign((void **)&temp_rdma, 4096,
				sizeof(struct rdma_region));

		temp_send->buffer.seq_id = i;
		temp_recv->buffer.seq_id = i;
		temp_send->buffer.seq_num = 0;
		temp_recv->buffer.seq_num = 0;
		temp_send->buffer.crc32 = 0;
		temp_recv->buffer.crc32 = 0;
		temp_rdma->seq_id = i;

		if (s_page_mode == PAGE_HUGE)
		  temp_rdma->data_buffer =
		    gAllocator.alloc(s_rdma_buffer_size *
				     (s_mode == M_VERIFY ? 2 : 1));
		else // GPUDirect-ATUL
		  /* CPU or GPU memory buffer allocation */
		  /* CPU Allocatoin */
		  /*		temp_rdma->data_buffer = malloc(s_rdma_buffer_size *
						  (s_mode == M_VERIFY ? 2 : 1));
                  */
		  /* GPU */		
		  gpuBuff = (void*) work_buffer_alloc(4096, 1, "d8:00.0"); // BDF HardCoded
		if (!gpuBuff) {
		  return -1; // fixme ATUL
		  //ret_val = 1;
		  //goto clean_device;
		}
		temp_rdma->data_buffer = gpuBuff;
		temp_rdma->conn = conn;
		temp_rdma->v_stage = VERIFY_STAGE1;
		DEBUG_PRINT("temp_rdma buffer address: %p\n",
			    temp_rdma->data_buffer);

		TEST_Z(temp_send->send_local_mr = ibv_reg_mr(
			s_ctx->pd,
			&temp_send->buffer,
			sizeof(struct message),
			IBV_ACCESS_LOCAL_WRITE));

		TEST_Z(temp_recv->send_local_mr = ibv_reg_mr(
			s_ctx->pd,
			&temp_recv->buffer,
			sizeof(struct message),
			IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_WRITE |
			IBV_ACCESS_REMOTE_READ));

		for (j = 0; j < (s_mode == M_VERIFY ? 2 : 1); j++) {
			TEST_Z(temp_rdma->data_mr[j] = ibv_reg_mr(
				s_ctx->pd,
				temp_rdma->data_buffer + j * s_rdma_buffer_size,
				s_rdma_buffer_size,
				IBV_ACCESS_LOCAL_WRITE |
				IBV_ACCESS_REMOTE_WRITE |
				IBV_ACCESS_REMOTE_READ));
		}

	  TAILQ_INSERT_TAIL(&conn->send_free_head, temp_send, next);
	  TAILQ_INSERT_TAIL(&conn->recv_free_head, temp_recv, next);
	  TAILQ_INSERT_TAIL(&conn->rdma_free_head, temp_rdma, next);
	}

#if EXTRA_RECVS
	for (i = 0; i < MIN_POST_RECV; i++) {
		posix_memalign((void **)&temp_recv, 4096,
				sizeof(struct recv_msg));
		TEST_Z(temp_recv->send_local_mr = ibv_reg_mr(
			s_ctx->pd,
			&temp_recv->buffer,
			sizeof(struct message),
			IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_WRITE |
			IBV_ACCESS_REMOTE_READ));

		TAILQ_INSERT_TAIL(&conn->recv_free_head, temp_recv, next);
	}
#endif
}

void do_rdma_op(void *context, struct rdma_region **rdma_ptr)
{
	struct connection *conn = (struct connection *)context;
	struct ibv_send_wr *bad_wr = NULL;
	struct rdma_region *temp_rdma = *rdma_ptr;

	temp_rdma->wr.wr_id = (uintptr_t)&temp_rdma->wr;
	if (s_mode == M_VERIFY)
		temp_rdma->wr.opcode = (temp_rdma->v_stage == VERIFY_STAGE1) ?
				       IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE;
	else
		temp_rdma->wr.opcode = (s_mode == M_WRITE) ?
				       IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;

	temp_rdma->wr.sg_list = temp_rdma->sge;
	temp_rdma->wr.num_sge = s_sge;
	temp_rdma->wr.send_flags = IBV_SEND_SIGNALED;
	temp_rdma->conn = conn;

	if (temp_rdma->v_stage == VERIFY_STAGE1) {
		// Read the top half of the data_buffer attrib
		temp_rdma->wr.wr.rdma.remote_addr =
				(uintptr_t)temp_rdma->peer_mr[0].addr;
		temp_rdma->wr.wr.rdma.rkey = temp_rdma->peer_mr[0].rkey;
	} else {
		// Write to the bottom half of the data_buffer attrib
		temp_rdma->wr.wr.rdma.remote_addr =
				(uintptr_t)temp_rdma->peer_mr[1].addr;
		temp_rdma->wr.wr.rdma.rkey = temp_rdma->peer_mr[1].rkey;
	}

	populate_sge(temp_rdma);
	TEST_NZ(ibv_post_send(conn->qp, &temp_rdma->wr, &bad_wr));
}

void send_message(struct connection *conn, struct send_msg **send_msg_ptr)
{
	struct ibv_send_wr *bad_wr = NULL;
	struct send_msg *temp_send = *send_msg_ptr;
	uint32_t crc = 0;

	temp_send->conn = conn;
	temp_send->wr.wr_id = (uintptr_t)&temp_send->wr;
	temp_send->wr.next = NULL;
	temp_send->wr.opcode = IBV_WR_SEND;
	temp_send->wr.sg_list = &temp_send->sge;
	temp_send->wr.num_sge = 1;
	temp_send->wr.send_flags = IBV_SEND_SIGNALED;

	temp_send->sge.addr = (uintptr_t)&temp_send->buffer;
	temp_send->sge.length = sizeof(struct message);
	temp_send->sge.lkey = temp_send->send_local_mr->lkey;

	temp_send->buffer.crc32 = 0;
	crc = crc32(0, (uint8_t *)&temp_send->buffer,
		    sizeof(struct message));
	temp_send->buffer.crc32 = crc;

	while (!conn->connected)
		sleep(0.2);

	TEST_NZ(ibv_post_send(conn->qp, &temp_send->wr, &bad_wr));
}

void send_mr(void *context)
{
	struct connection *conn = (struct connection *)context;
	struct send_msg *temp_send = NULL;
	struct rdma_region *temp_rdma = TAILQ_FIRST(&conn->rdma_free_head);
	int i = 0, j = 0, index = 1;

	srand(time(NULL));
	TEST_NZ(pthread_spin_lock(&conn->send_free_lock));
	for (j = 0; j < s_qdepth; j++) {
		temp_send = TAILQ_FIRST(&conn->send_free_head);
		if (temp_send == NULL) {
			printf("%s: No nodes in send_free head\n", __func__);
			die();
		}

		TAILQ_REMOVE(&conn->send_free_head, temp_send, next);
		temp_send->buffer.msg_type = MSG_MR;
		temp_send->buffer.msg_op = s_mode;
		temp_send->buffer.seq_num = conn->send_msg_count + 1;
		for (i = 0; i < (s_mode == M_VERIFY ? 2 : 1); i++)
			memcpy(&temp_send->buffer.data.mr[i],
			       temp_rdma->data_mr[i],
			       sizeof(struct ibv_mr));

		if (s_mode == M_VERIFY)
			fill_pattern(&temp_rdma);

		send_message(conn, &temp_send);
		TAILQ_INSERT_TAIL(&conn->send_posted_head, temp_send, next);
		temp_rdma = TAILQ_NEXT(temp_rdma, next);
		conn->send_msg_count++;
		++index;
		DEBUG_PRINT("MR sent, QP|Seq num = 0x%x|%d\n",
			conn->qp->qp_num,
			conn->send_msg_count);
	}
	TEST_NZ(pthread_spin_unlock(&conn->send_free_lock));
}

void complete_iteration(void *context)
{
#if QP_LOCK_STEP
	struct rdma_cm_id **id_array = (struct rdma_cm_id **)s_conn_array;
	int i;

	TEST_NZ(pthread_spin_lock(&s_ctx->global_lock));
	s_ctx->send_archived += 1;

	if (s_ctx->send_archived == s_num_qps) {
		for (i = 0; i < s_num_qps; i++) {
			send_mr(id_array[i]->context);
			s_ctx->send_archived = 0;
		}
	}
	TEST_NZ(pthread_spin_unlock(&s_ctx->global_lock));
#else
	send_mr(context);
#endif
}

/*
 * void generate_sge_distribution()
 * {
 *	int i;
 *	int blocksize_remaining = s_rdma_buffer_size;
 *	int blocksize_sge = blocksize_remaining / s_sge;
 *	int blocksize_rem = blocksize_remaining % s_sge;
 *	s_sge_distribution = malloc(sizeof(int) * s_sge);
 *
 *	if(blocksize_rem != 0)
 *		blocksize_sge += 1;
 *
 *	for(i = 0; i < s_sge; i++) {
 *		s_sge_distribution[i] = (blocksize_remaining > blocksize_sge) ?
 *					blocksize_sge : blocksize_remaining;
 *		blocksize_remaining -= blocksize_sge;
 *	}
 * }
 */

void populate_sge(struct rdma_region *temp_rdma)
{
	uint32_t sge_size, sge_left, i;

	// temp_rdma->message.rdma_size;
	sge_left = s_rdma_buffer_size;
	// 100 / 7 = 14, 14 / 4 = 12, 12 * 6 = 72, 7th is 28 bytes
	sge_size = sge_left / s_sge;
	sge_size = sge_size / 4;
	sge_size = sge_size * 4;

	for (i = 0; i < s_sge; i++) {
		temp_rdma->sge[i].addr =
			(uintptr_t)(temp_rdma->data_buffer + sge_size * i);
		temp_rdma->sge[i].lkey = temp_rdma->data_mr[0]->lkey;

		if (i == (s_sge - 1)) {
			temp_rdma->sge[i].length = sge_left;
			sge_left = 0;
		} else {
			temp_rdma->sge[i].length = sge_size;
			sge_left = sge_left - sge_size;
		}
		//printf("SGE: 0x%016lX %d\n", temp_rdma->sge[i].addr,
		//	temp_rdma->sge[i].length);
	}
	/*
	 * sge_left = send_ptr->message.rdma_size;
	 * sge_size = sge_left / g_num_sge;
	 *
	 * if (sge_left % g_num_sge)
	 *	sge_size++;
	 *
	 * for (i=0; i<g_num_sge; i++) {
	 *	send_ptr->sge[i].addr = (uintptr_t)(send_ptr->data1 +
	 *				sge_size * i);
	 *	send_ptr->sge[i].lkey = send_ptr->local_data_mr[0]->lkey;
	 *	send_ptr->sge[i].length = sge_left < sge_size ?
	 *				  sge_left : sge_size;
	 *	sge_left = sge_left < sge_size ? 0 : (sge_left - sge_size);
	 * }
	 */
}

void set_operating_mode(enum queue_mode q, enum page_mode p)
{
	s_cpu_count = get_nprocs_conf();
	s_queue_mode = q;
	s_page_mode = p;
}

void set_user_data(struct user_data u_data)
{
	s_qdepth = u_data.qdepth;
	s_sge = u_data.no_sge;
	s_mode = u_data.rdma_op;
	s_rdma_buffer_size = u_data.blocksize;
}

void set_test_params(uint64_t loop, enum role r, enum error_handling on_error)
{
	s_role = r;
	s_loops = loop;
	s_on_error = on_error;
}

void release_cq_lock(void)
{
	TEST_NZ(pthread_spin_unlock(&s_ctx->cq_lock));
}

void create_qp_handler(struct rdma_cm_id **conn_array)
{
	s_conn_array = conn_array;

	TEST_NZ(pthread_create(&s_ctx->qp_handler_thread,
		NULL, poll_qps, (void *)conn_array));
}

void set_pattern(uint64_t pattern, enum verify_op v_op)
{
	s_verify_mode = v_op;
	s_verify_pattern = pattern;
	s_verify_pattern = (s_verify_pattern << 32) | s_verify_pattern;
}

void start_bw_timer(void)
{
	gettimeofday(&s_start, NULL);
}

void stop_bw_timer(void)
{
	gettimeofday(&s_stop, NULL);
}

void set_qdepth(uint16_t qdepth)
{
	s_qdepth = qdepth;
}
