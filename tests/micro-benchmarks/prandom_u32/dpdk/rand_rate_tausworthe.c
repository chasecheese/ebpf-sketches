/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/queue.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/random.h>
#include <semaphore.h>

#include "../prandom.h"
#include "../utils.h"

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#define CONTROL_VALUE 1234

#ifndef _NUM_RAND_CYCLES
#define _NUM_RAND_CYCLES 4
#endif

#ifdef DEBUG
#define DEBUG_TEST 1
#else
#define DEBUG_TEST 0
#endif

#define debug_print(...) \
            do { if (DEBUG_TEST) fprintf(stderr, __VA_ARGS__); } while (0)

struct rnd_state net_rand_state;

struct thread_ctx {
    sem_t*              sem_stop;
    unsigned int        thread_id;
	struct timespec 	tstart;
	struct timespec 	tend;
};

u32 prandom_u32_state(struct rnd_state *state)
{
#define TAUSWORTHE(s, a, b, c, d) ((s & c) << d) ^ (((s << a) ^ s) >> b)
	state->s1 = TAUSWORTHE(state->s1,  6U, 13U, 4294967294U, 18U);
	state->s2 = TAUSWORTHE(state->s2,  2U, 27U, 4294967288U,  2U);
	state->s3 = TAUSWORTHE(state->s3, 13U, 21U, 4294967280U,  7U);
	state->s4 = TAUSWORTHE(state->s4,  3U, 12U, 4294967168U, 13U);

	return (state->s1 ^ state->s2 ^ state->s3 ^ state->s4);
}


/**
 *	prandom_u32 - pseudo random number generator
 *
 *	A 32 bit pseudo-random number is generated using a fast
 *	algorithm suitable for simulation. This algorithm is NOT
 *	considered safe for cryptographic use.
 */
u32 prandom_u32(void)
{
	struct rnd_state *state = (&net_rand_state);
	u32 res = prandom_u32_state(state);
	//put_cpu_ptr(&net_rand_state);
	return res;
}

static void prandom_warmup(struct rnd_state *state)
{
	/* Calling RNG ten times to satisfy recurrence condition */
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
}

static int prandom_seed_full_state(){
	int i=0;
	struct rnd_state *state = (&net_rand_state);
	u32 seeds[4];

	int ret = getrandom(&seeds, sizeof(seeds), 0);
	if (ret < 0 || ret != sizeof(seeds)) {
		debug_print("Error in getting random numbers\n");
		return -1;
	}

	state->s1 = __seed(seeds[0],   2U);
	state->s2 = __seed(seeds[1],   8U);
	state->s3 = __seed(seeds[2],  16U);
	state->s4 = __seed(seeds[3], 128U);

	prandom_warmup(state);
	return 0;
}

static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.max_rx_pkt_len = RTE_ETHER_MAX_LEN,
	},
};


volatile uint64_t tmp_idx = 0;

uint64_t rdtsc(){
    unsigned int lo,hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	struct rte_ether_addr addr;
	const uint16_t rx_rings = 1, tx_rings = 1;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		debug_print("Error during getting device (port %u) info: %s\n",
				port, strerror(-retval));
		return retval;
	}

	if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |=
			DEV_TX_OFFLOAD_MBUF_FAST_FREE;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
				rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;

	debug_print("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}

static int lcore_hello(void* thread_ctx)
{
	struct thread_ctx*  ctx;
	debug_print("\nHello forwarding packets. ");
	uint16_t port;
	int sem_value = 0;

	if (!thread_ctx)
        return (EINVAL);

	/* retrieve thread context */
    ctx = (struct thread_ctx*)thread_ctx;
	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	RTE_ETH_FOREACH_DEV(port)
		if (rte_eth_dev_socket_id(port) >= 0 &&
			rte_eth_dev_socket_id(port) != (int)rte_socket_id()) {
			debug_print("\n\n");
			debug_print("WARNING: port %u is on remote NUMA node\n",
			       port);
			debug_print("to polling thread.\n");
			debug_print("Performance will not be optimal.\n");
		}
	debug_print("\nCore %u forwarding packets. ", rte_lcore_id());
	debug_print("\nRunning with %u rand calls in cycle.\n", _NUM_RAND_CYCLES);
	debug_print("[Ctrl+C to quit]\n");
	uint32_t idx;
	const uint64_t N = _NUM_RAND_CYCLES;

	if (prandom_seed_full_state() < 0) {
		exit(1);
	}

	struct timespec tstart = {0, 0}, tend = {0, 0};
  	clock_gettime(CLOCK_MONOTONIC, &tstart);

	while (!sem_value) {
		RTE_ETH_FOREACH_DEV(port) {
			/* Get burst of RX packets, from first port of pair. */
			struct rte_mbuf *bufs[BURST_SIZE];
			const uint16_t nb_rx = rte_eth_rx_burst(port, 0,
					bufs, BURST_SIZE);

			if (unlikely(nb_rx == 0))
				continue;

			for (int i=0; i < nb_rx; i++) {
				volatile __u32 rndval = 0;
    			__u32 atom;

				struct rte_mbuf *pkt = bufs[i];
				
				for (int i = 0; i < N; i++) {
        			atom=prandom_u32();
        			rndval^=atom;
    			}

				if (rndval == CONTROL_VALUE) {
					rte_pktmbuf_free(pkt);
					continue;
				}
			}

			/* Send burst of TX packets, to second port of pair. */
			const uint16_t nb_tx = rte_eth_tx_burst(port ^ 1, 0, bufs, nb_rx);

			/* Free any unsent packets. */
			if (unlikely(nb_tx < nb_rx)) {
				uint16_t buf;

				for (buf = nb_tx; buf < nb_rx; buf++)
					rte_pktmbuf_free(bufs[buf]);		
			}

			sem_getvalue(ctx->sem_stop, &sem_value);
            if (sem_value > 0) {
				debug_print("Received STOP signal\n");
				rte_eth_dev_stop(port);

				clock_gettime(CLOCK_MONOTONIC, &tend);

				ctx->tstart = tstart;
				ctx->tend = tend;
                break;
            }		
		}
	}

	return (0);
}

int
main(int argc, char **argv)
{
	int ret;
	unsigned lcore_id;
	struct thread_ctx ctx;
	struct rte_mempool *mbuf_pool;
	uint16_t nb_ports;
	uint16_t portid;
	int socket_id;
	sem_t sem_stop;

#ifndef DEBUG
	rte_log_set_global_level(RTE_LOG_EMERG);
#endif

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports < 2 || (nb_ports & 1))
		rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initialize all ports. */
	RTE_ETH_FOREACH_DEV(portid)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n",
					portid);

	if (rte_lcore_count() > 1)
		debug_print("\nWARNING: Too many lcores enabled. Only 1 used.\n");

	socket_id = rte_eth_dev_socket_id(0);

	if (sem_init(&sem_stop, 0, 0)) {
        fprintf(stderr, "sem_init failed: %s\n", strerror(errno));
        return (errno);
    }

	ctx.sem_stop = &sem_stop;

	debug_print("Launching remote thread\n");
	ret = rte_eal_mp_remote_launch(lcore_hello, &ctx, 0); /* skip fake master core */
	if (ret) {
		fprintf(stderr, "rte_eal_remote_launch failed: %s\n", strerror(ret));
		return (ret);
	}

	/* call it on main lcore too */
	// lcore_hello(NULL);
	
	for(int i = 0; i < 60; i++) {
		debug_print("Running %d/60\n", i);
		sleep(1);
	}
	ret = sem_post(&sem_stop);
	if (ret) {
		fprintf(stderr, "sem_post failed: %s\n", strerror(errno));
		return (errno);
	}

	rte_eal_mp_wait_lcore();

	struct rte_eth_stats stats;
	ret = rte_eth_stats_get(1, &stats);
	if (ret) {
		debug_print("Error while reading stats from port: %u\n", 1);
	} else {
		debug_print("-> Stats for port: %u\n\n", 1);
		debug_print("%u,%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64"\n", 
				1,
				stats.ipackets,
				stats.ibytes,
				stats.opackets,
				stats.obytes);
	}

	debug_print("Rand calc took about %.5f seconds\n", ((double)ctx.tend.tv_sec + 1.0e-9 * ctx.tend.tv_nsec) -
           										  ((double)ctx.tstart.tv_sec + 1.0e-9 * ctx.tstart.tv_nsec));

  	double timediff = ((double)ctx.tend.tv_sec + 1.0e-9 * ctx.tend.tv_nsec) -
                      ((double)ctx.tstart.tv_sec + 1.0e-9 * ctx.tstart.tv_nsec);

	double randrate_hs = (stats.opackets*_NUM_RAND_CYCLES)/timediff;
	double randrate_Mhs = randrate_hs / 1e6;
	debug_print("Random generation rate: %.4f Mrand/s\n", randrate_Mhs);

	printf("%.4f", randrate_Mhs);

	/* clean up the EAL */
	rte_eal_cleanup();

	return 0;
}
