/*
 *   BSD LICENSE
 *
 *   Copyright (C) Cavium 2017.
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
 *     * Neither the name of Cavium networks nor the names of its
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

#ifndef _TEST_ORDER_COMMON_
#define _TEST_ORDER_COMMON_

#include <stdio.h>
#include <stdbool.h>

#include <rte_cycles.h>
#include <rte_eventdev.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>

#include "evt_common.h"
#include "evt_options.h"
#include "evt_test.h"

#define BURST_SIZE 16

struct test_order;

struct worker_data {
	uint8_t dev_id;
	uint8_t port_id;
	struct test_order *t;
};

struct prod_data {
	uint8_t dev_id;
	uint8_t port_id;
	uint8_t queue_id;
	struct test_order *t;
};

struct test_order {
	/* Don't change the offset of "err". Signal handler use this memory
	 * to terminate all lcores work.
	 */
	int err;
	/*
	 * The atomic_* is an expensive operation,Since it is a functional test,
	 * We are using the atomic_ operation to reduce the code complexity.
	 */
	rte_atomic64_t outstand_pkts;
	enum evt_test_result result;
	uint32_t nb_flows;
	uint64_t nb_pkts;
	struct rte_mempool *pool;
	struct prod_data prod;
	struct worker_data worker[EVT_MAX_PORTS];
	uint32_t *producer_flow_seq;
	uint32_t *expected_flow_seq;
	struct evt_options *opt;
} __rte_cache_aligned;

static inline int
order_nb_event_ports(struct evt_options *opt)
{
	return evt_nr_active_lcores(opt->wlcores) + 1 /* producer */;
}

int order_test_result(struct evt_test *test, struct evt_options *opt);
int order_opt_check(struct evt_options *opt);
int order_test_setup(struct evt_test *test, struct evt_options *opt);
int order_mempool_setup(struct evt_test *test, struct evt_options *opt);
int order_event_dev_port_setup(struct evt_test *test, struct evt_options *opt,
				uint8_t nb_workers, uint8_t nb_queues);
void order_test_destroy(struct evt_test *test, struct evt_options *opt);
void order_opt_dump(struct evt_options *opt);
void order_mempool_destroy(struct evt_test *test, struct evt_options *opt);
void order_eventdev_destroy(struct evt_test *test, struct evt_options *opt);

#endif /* _TEST_ORDER_COMMON_ */