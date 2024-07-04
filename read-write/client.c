// SPDX-License-Identifier: GPL-2.0

#include <cuda.h>
#include <cuda_runtime.h>

#include "common.h"
#include "gpu_mem_util.h"

#define PARALLEL_QPS_CONNECT	50

const int TIMEOUT_IN_MS = 500; /* ms */
static enum queue_mode client_qmode = QUEUE_NORMAL;
static enum page_mode client_pmode = PAGE_DEFAULT;
static unsigned int client_qps = 1;
static unsigned int client_cqs = 1;
static unsigned int qps_resolved;
static unsigned int qps_connected;
static unsigned int qps_disconnected;

struct addrinfo *addr;
struct rdma_cm_event *event;
struct rdma_cm_id **conn_array;
struct rdma_event_channel *ec;
char *default_server_ip_addr = "8.8.8.114";
char default_server_port[20] = "11235";

static int on_addr_resolved(struct rdma_cm_id *id);
static int on_connection(struct rdma_cm_event *event);
static int on_disconnect(struct rdma_cm_id *id);
static int on_event(struct rdma_cm_event *event, void *user_data);
static int on_route_resolved(struct rdma_cm_id *id, void *user_data);
static void initiate_connect(int qp_index);
static void usage(const char *argv0);

int main(int argc, char **argv)
{
	struct user_data u_data = {.rdma_op = M_READ, .qdepth = 1,
				   .no_sge = 1, .blocksize = 1024};
	uint64_t default_verify_pattern = 0x12345678;
	uint64_t loops = MIN_LOOPS;
	char *pattern_input;
	enum verify_op pattern_type = VERIFY_PATTERN;
	enum error_handling on_error = DIE_ON_ERROR;
	int opt, i;     

	if (argc < 2)
		usage(argv[0]);

	while ((opt = getopt(argc, argv, "rwhekjv:q:b:s:l:i:p:t:y:")) != -1) {
		switch (opt) {
		case 'r':
			u_data.rdma_op = M_READ;
			break;
		case 'w':
			u_data.rdma_op = M_WRITE;
			break;
		case 'v':
			u_data.rdma_op = M_VERIFY;
			pattern_input = optarg;
			break;
		case 'q':
			u_data.qdepth = strtoul(optarg, NULL, 10);
			break;
		case 'b':
			u_data.blocksize = strtoull(optarg, NULL, 10);
			break;
		case 's':
			u_data.no_sge = strtoul(optarg, NULL, 10);
			break;
		case 'l':
			loops = strtoul(optarg, NULL, 10);
			loops = (loops > MAX_LOOPS) ? MAX_LOOPS :
				((loops < MIN_LOOPS) ? MIN_LOOPS : loops);
			break;
		case 't':
			client_qps = strtoul(optarg, NULL, 10);
			client_qps = (client_qps > MAX_QP) ?
				     MAX_QP : ((client_qps < MIN_QP) ?
				     MIN_QP : client_qps);
			break;
		case 'y':
			client_cqs = strtoul(optarg, NULL, 10);
			client_cqs = (client_cqs > MAX_QP) ?
				     MAX_QP : ((client_cqs < MIN_QP) ?
				     MIN_QP : client_cqs);
			break;
		case 'k':
			client_qmode = QUEUE_SHARED;
			break;
		case 'j':
			client_pmode = PAGE_HUGE;
			break;
		case 'e':
			on_error = HANG_ON_ERROR;
			break;
		case 'h':
			usage(argv[0]);
			break;
		}
	}

	event = NULL;
	ec = NULL;
	qps_resolved = 0;
	qps_connected = 0;
	qps_disconnected = 0;

	set_operating_mode(client_qmode, client_pmode);
	set_qdepth(u_data.qdepth);

	/* Sanity Checks for SGE and blocksize */
	u_data.no_sge = (u_data.no_sge > MAX_SGE ?
			MAX_SGE : ((u_data.no_sge < MIN_SGE) ?
			MIN_SGE : u_data.no_sge));
	u_data.blocksize = (u_data.blocksize > MAX_BLOCKSIZE ?
			MAX_BLOCKSIZE : ((u_data.blocksize < MIN_BLOCKSIZE) ?
			MIN_BLOCKSIZE : u_data.blocksize));

	if (!strcmp(pattern_input, "random")) {
		pattern_type = VERIFY_RANDOM;
		default_verify_pattern = rand();
	} else {
		pattern_type = VERIFY_PATTERN;
		default_verify_pattern = strtoull(pattern_input, NULL, 16);
	}

	initialize_queues(client_qps, client_cqs);
	set_test_params(loops, ROLE_CLIENT, on_error);
	set_pattern(default_verify_pattern, pattern_type);

	/* Printing client CLI args */
	printf("Server IP Address: %s\n", default_server_ip_addr);
	printf("Server IP Port: %s\n", default_server_port);
	(u_data.rdma_op == M_READ) ? printf("Operation: Read\n") :
				     printf("Operation: Write\n");
	printf("client qdepth(pre-negotiation): %d\n", u_data.qdepth);
	printf("client blocksize: %d\n\n", u_data.blocksize);

	/* Build array of rdma_cm_id */
	conn_array = malloc(sizeof(struct rdma_cm_id *) * client_qps);
	TEST_Z(ec = rdma_create_event_channel());

	/* Connect only a batch of PARALLEL_QPS_CONNECT QPs at a time.
	 * Because soft rxe has a max AH limit of 100 at a time,
	 * the client needs to connect a max of only 100 QPs first, wait for
	 * all those AHs to get destroyed and then connect the next 100
	 */
	for (i = 0; i < client_qps && i < PARALLEL_QPS_CONNECT; i++)
		initiate_connect(i);
	printf ("[%d][%s] Atul 1 \n", __LINE__, __func__);
	while (rdma_get_cm_event(ec, &event) == 0) {
		struct rdma_cm_event event_copy;

		memcpy(&event_copy, event, sizeof(*event));
		rdma_ack_cm_event(event);
		printf ("[%d][%s] Atul 2 \n", __LINE__, __func__);
		if (on_event(&event_copy, (void *)&u_data))
			break;
	}

	rdma_destroy_event_channel(ec);

	return 0;
}

int on_addr_resolved(struct rdma_cm_id *id)
{
	DEBUG_PRINT("address resolved.\n");
	/* NOTE:
	 *  You would have to run
	 * build_context only once.
	 * This is because rdma_cm_ids
	 * are dependent on the singular context
	 * which in this case is the ibv_device
	 * which will remain for subsequent
	 * cm_id structures
	 */
	if (qps_resolved == 0)
		build_context(id->verbs);

	build_connection(id);
	TEST_NZ(rdma_resolve_route(id, TIMEOUT_IN_MS));

	qps_resolved += 1;
	return 0;
}

int on_connection(struct rdma_cm_event *event)
{
	struct rdma_conn_param recv_param = event->param.conn;
	struct user_data recv_data =
			*(struct user_data *)(recv_param.private_data);
	int i;

	on_connect(event->id->context);
	DEBUG_PRINT("received qdepth from server: %d\n", recv_data.qdepth);
	set_user_data(recv_data);
	reg_mem_and_post_recv(event->id);
	send_mr(event->id->context);

	qps_connected += 1;
	DEBUG_PRINT("Number of QPS connected = %d, QP num = %d\n",
		qps_connected, event->id->qp->qp_num & 0xFFFF);

	if (qps_connected == client_qps) {
		printf("All qps connected.\n");
		release_cq_lock();
		freeaddrinfo(addr);
		create_qp_handler(conn_array);
		return 0;
	// Once PARALLEL_QPS_CONNECT QPs are connected, connect another batch
	} else if (qps_connected % PARALLEL_QPS_CONNECT == 0) {
		for (i = qps_connected;
		     i < client_qps &&
		     i < (qps_connected + PARALLEL_QPS_CONNECT);
		     i++)
			initiate_connect(i);
	}

	return 0;
}

void initiate_connect(int qp_index)
{
	sprintf(default_server_port, "%lu",
		(unsigned long)SERVER_PORT_BASE + qp_index);
	TEST_NZ(getaddrinfo(default_server_ip_addr, default_server_port,
			    NULL, &addr));
	TEST_NZ(rdma_create_id(ec, &conn_array[qp_index],
			       NULL, RDMA_PS_TCP));
	TEST_NZ(rdma_resolve_addr(conn_array[qp_index], NULL,
				  addr->ai_addr, TIMEOUT_IN_MS));
}

int on_disconnect(struct rdma_cm_id *id)
{
	destroy_connection(id->context);
	qps_disconnected += 1;

	if (qps_disconnected == client_qps) {
		printf("All qps disconnected.\n");
		gAllocator.close();
		destroy_context();
		return 1;
	}

	return 0; /* exit event loop */
}

int on_event(struct rdma_cm_event *event, void *user_data)
{
	int r = 0;

	if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED) {
		r = on_addr_resolved(event->id);
		printf ("[%d][%s] Atul r=[%d] \n", __LINE__, __func__, r);
	}
	else if (event->event == RDMA_CM_EVENT_ROUTE_RESOLVED) {
		r = on_route_resolved(event->id, user_data);
		printf ("[%d][%s] Atul r=[%d] \n", __LINE__, __func__, r);
	}
	else if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
		r = on_connection(event);
		printf ("[%d][%s] Atul r=[%d] \n", __LINE__, __func__, r);
	}
	else if (event->event == RDMA_CM_EVENT_DISCONNECTED) {
		r = on_disconnect(event->id);
		printf ("[%d][%s] Atul r=[%d] \n", __LINE__, __func__, r);
	}
	else {
		fprintf(stderr, "%s: %d\n", __func__, event->event);
		printf ("[%d][%s] Atul 2 \n", __LINE__, __func__);
		printf("%s: unknown event.\n", __func__);
		die();
	}

	return r;
}

int on_route_resolved(struct rdma_cm_id *id, void *user_data)
{
	struct rdma_conn_param cm_params;

	DEBUG_PRINT("route resolved.\n");
	build_params(&cm_params, user_data);
	TEST_NZ(rdma_connect(id, &cm_params));

	return 0;
}

void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("%s <actions> [-<parameter-flag> <parameter-args>]...\n", argv0);
	printf("%s {-r|-w|-v {\"random\"/\"<pattern>\"}} [-q Qdepth>] ", argv0);
	printf("[-s Num of SGEs] [-l Iterations] [-b Blocksize] ");
	printf("[-t Num of QPs] [-y Num of CQs]\n\n");
	printf("Actions:\n");
	printf("	Reads: -r\n");
	printf("	Writes: -w\n");
	printf("	Verify: -v <pattern>/\"random\"\n");
	printf("		Eg. -v 0x12345678 / -v random\n\n");
	printf("Parameters:\n");
	printf("	Queue Depth: -q <qdepth>\n");
	printf("	No of SGEs: -s <sges>\n");
	printf("	Queue Pairs: -t <qp>\n");
	printf("	Completion Queues: -y <cqs>\n");
	printf("	Shared Receive Queues: -k\n");
	printf("	Huge Page mode: -j\n");

	exit(1);
}
