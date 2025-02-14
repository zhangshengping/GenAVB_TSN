/*
* Copyright 2021 NXP
*
* NXP Confidential. This software is owned or controlled by NXP and may only
* be used strictly in accordance with the applicable license terms.  By expressly
* accepting such terms or by downloading, installing, activating and/or otherwise
* using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be
* bound by the applicable license terms, then you may not retain, install, activate
* or otherwise use the software.
*/

/**
 @file
 @brief Linux AF_XDP Network service implementation
 @details
*/


#define _GNU_SOURCE

 #include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <poll.h>
#include <string.h>

#define asm __asm__
#include <bpf/libbpf.h>
#include <bpf/xsk.h>
#include <bpf/bpf.h>
#include <linux/if_link.h>

#include "common/log.h"
#include "common/net.h"
#include "common/list.h"
#include "epoll.h"
#include "net_logical_port.h"
#include "net.h"
#include "pool.h"

#define NET_XDP_ZEROCOPY 1

#define DEFAULT_XDP_QUEUE 0

// Derived from avbdrv.h
#define BUF_ORDER	11
#define BUF_SIZE	(1 << BUF_ORDER)
#define BUF_HEADROOM	64
#define NET_DATA_OFFSET 128 /* Hardcode the value to 128 as a common L1_CACHE alignment of
                               the max avb descriptor size for both i.MX6 and i.MX8*/

#define TX_QUEUE_SIZE		32
#define RX_QUEUE_SIZE		32
#if defined(NET_XDP_ZEROCOPY)
#define HW_RX_QUEUE_SIZE	512
#else
#define HW_RX_QUEUE_SIZE	0
#endif
#define N_QUEUES		1
/* The FILL queue must be deep enough to accommodate:
 * . the buffers that will be placed in the HW queue when the UMEM is created (HW_RX_QUEUE_SIZE)
 * . the buffers that will land in the RX queue as the HW starts receiving packets (RX_QUEUE_SIZE)
 * . some amount of buffers to be used by the kernel to refill the HW queue once the application
 *   starts dequeuing packets from the Rx queue (RX_QUEUE_SIZE).
 *   => so the FILL queue must contain at least HW_RX_QUEUE_SIZE + 2*RX_QUEUE_SIZE buffers, and be
 *   a power of 2 (design constraint).
 *
 *   For a given queue, the buffer pool must hold:
 *   . the packets that go into the FILL queue when the umem is created (FILL_SIZE)
 *   . some packets to be used by the refill code to keep the FILL queue from emptying itself (RX_QUEUE_SIZE)
 *   . the packets that may go into the TX queue (TX_QUEUE_SIZE)
 *   . the packets already transmitted but not recovered yet by user-space (COMPLETION_QUEUE_SIZE)
 */
#define FILL_QUEUE_SIZE		(2*max(HW_RX_QUEUE_SIZE, 2*RX_QUEUE_SIZE))  // Must be a power of 2, bigger than the sum of both
#define FILL_LEVEL_INITIAL	(HW_RX_QUEUE_SIZE + 2*RX_QUEUE_SIZE)
#define FILL_LEVEL		(RX_QUEUE_SIZE)
#define COMPLETION_QUEUE_SIZE	(TX_QUEUE_SIZE)
#define BUFFERS_MAX		(N_QUEUES*(FILL_LEVEL_INITIAL + RX_QUEUE_SIZE + COMPLETION_QUEUE_SIZE + TX_QUEUE_SIZE))
#define BUF_POOL_SIZE		(BUFFERS_MAX * BUF_SIZE)

#define GENAVB_XDPKEY_NAME "/sys/fs/bpf/xdp/globals/genavb_xdpkey"
#define GENAVB_XSKMAP_NAME "/sys/fs/bpf/xdp/globals/genavb_xskmap"

struct net_xdp_umem {
	struct xsk_umem *umem;
	struct xsk_ring_prod fill_ring;
	struct xsk_ring_cons completion_ring;
	pthread_mutex_t fill_lock;
	pthread_mutex_t completion_lock;
	struct list_head list;
	unsigned int queue_index;
	unsigned int refcnt;
};

struct net_xdp_ctx {
	struct net_xdp_umem *umem;
	struct xsk_socket *xdpsock;
	struct xsk_ring_cons rx_queue;
	struct xsk_ring_prod tx_queue;
	struct net_address addr;
};

static struct list_head umem_list;
pthread_mutex_t umem_lock;

static void *umem_buffer_pool_area;
static struct pool umem_buffer_pool;

#define BMAP_LOGSIZE	6
#define BMAP_SIZE	(1 << BMAP_LOGSIZE)
#define BMAP_ARRAY_LEN	(MAX_SOCKETS / BMAP_SIZE)
static uint64_t free_index_bmap[BMAP_ARRAY_LEN];

static int xskmap_fd = -1;
static int xdpkey_fd = -1;

static struct os_xdp_config xdp_config;


static inline int get_unique_index(uint32_t *idx)
{
	unsigned int i = 0;

	pthread_mutex_lock(&umem_lock);

	do {
		*idx = ffsl(free_index_bmap[i]);
		i++;
	} while ((*idx == 0) && (i < BMAP_ARRAY_LEN));

	if (*idx == 0)
		return -1;

	i--;
	free_index_bmap[i] &= ~(1 << (*idx - 1));

	pthread_mutex_unlock(&umem_lock);

	*idx += i << BMAP_LOGSIZE;
	return 0;
}

static inline void release_unique_index(uint32_t index)
{
	unsigned int i;

	pthread_mutex_lock(&umem_lock);

	if (index > MAX_SOCKETS)
		goto err;

	i = (index & ~(BMAP_SIZE - 1)) >> BMAP_LOGSIZE;
	free_index_bmap[i] |= (1 << (index & (BMAP_SIZE - 1)));

err:
	pthread_mutex_unlock(&umem_lock);
}

static int net_xdp_xskmap_add_addr(struct net_address *addr, int xsk_fd)
{
	int rc;
	struct genavb_xdp_key key;
	uint32_t xsk;

	if ((xdpkey_fd == -1) || (xskmap_fd == -1))
		goto err_idx;

	key.protocol = addr->u.l2.protocol;
	key.vlan_id = addr->vlan_id;
	memcpy(key.dst_mac, addr->u.l2.dst_mac, 6);

	rc = get_unique_index(&xsk);
	if (rc < 0)
		goto err_idx;

	rc = bpf_map_update_elem(xdpkey_fd, &key, &xsk, 0);
	if (rc)
		goto err_xdp;

	rc = bpf_map_update_elem(xskmap_fd, &xsk, &xsk_fd, 0);
	if (rc)
		goto err_xsk;

	return 0;

err_xsk:
	bpf_map_delete_elem(xdpkey_fd, &key);
err_xdp:
	release_unique_index(xsk);
err_idx:
	return -1;
}

static int net_xdp_xskmap_del_addr(struct net_address *addr)
{
	struct genavb_xdp_key key;
	uint32_t xsk;
	int rc;

	if ((xdpkey_fd == -1) || (xskmap_fd == -1)) {
		os_log(LOG_ERR, "File descriptors for eBPF maps not initialized\n");
		return -1;
	}

	key.protocol = addr->ptype;
	key.vlan_id = addr->vlan_id;
	memcpy(key.dst_mac, addr->u.l2.dst_mac, 6);

	rc = bpf_map_lookup_elem(xdpkey_fd, &key, & xsk);
	if (!rc) {
		os_log(LOG_ERR, "Could not find XDP entry for key protocol(%d)\n", key.protocol);
		return -1;
	}

	bpf_map_delete_elem(xdpkey_fd, &key);
	bpf_map_delete_elem(xskmap_fd, &xsk);

	release_unique_index(xsk);

	return 0;
}

static void net_xdp_wakeup(int fd, const struct xsk_ring_prod *ring)
{
	if (!ring || xsk_ring_prod__needs_wakeup(ring)) {
		struct pollfd fds = {
				.fd = fd,
				.events = POLLIN,
		};

		poll(&fds, 1, 0);
	}
}

static void net_xdp_umem_fill_cleanup(struct net_xdp_umem *umem)
{
	unsigned int count;
	uint32_t idx = 0;
	const __u64 *addr, *start;

	pthread_mutex_lock(&umem->fill_lock);
	count = xsk_prod_nb_free(&umem->fill_ring, FILL_QUEUE_SIZE);
	count = xsk_ring_prod__reserve(&umem->fill_ring, 0, &idx);

	start = (void *)xsk_ring_prod__fill_addr(&umem->fill_ring, idx);
	idx += count;
	do {
		addr = xsk_ring_prod__fill_addr(&umem->fill_ring, idx);
		pool_free_shmem(&umem_buffer_pool, *addr);
		idx++;
	} while (addr != start);

	pthread_mutex_unlock(&umem->fill_lock);
}

static void net_xdp_umem_completion_cleanup(struct net_xdp_umem *umem)
{
	unsigned int count, i;
	const __u64 *addr;
	uint32_t idx = 0;

	pthread_mutex_lock(&umem->completion_lock);

	count = xsk_ring_cons__peek(&umem->completion_ring, COMPLETION_QUEUE_SIZE, &idx);

	for (i = 0; i < count; i++) {
		addr = xsk_ring_cons__comp_addr(&umem->completion_ring, idx + i);
		pool_free_shmem(&umem_buffer_pool, pool_align(&umem_buffer_pool, *addr));
	}
	xsk_ring_cons__release(&umem->completion_ring, count);

	pthread_mutex_unlock(&umem->completion_lock);
}

static struct net_xdp_umem *net_xdp_umem_create(unsigned int queue_index)
{
	struct xsk_umem_config cfg = {
		.fill_size = FILL_QUEUE_SIZE,
		.comp_size = COMPLETION_QUEUE_SIZE,
		.frame_size = BUF_SIZE,
		.frame_headroom = NET_DATA_OFFSET,
		.flags = XSK_UMEM__DEFAULT_FLAGS
	};
	struct net_xdp_umem *umem;
	int rc, i;
	uint32_t idx;


	umem = malloc(sizeof(struct net_xdp_umem));
	if (!umem) {
		os_log(LOG_ERR, "malloc() failed with error %s\n", strerror(errno));
		goto err_alloc;
	}

	rc = xsk_umem__create(&umem->umem, umem_buffer_pool_area, BUF_POOL_SIZE, &umem->fill_ring, &umem->completion_ring, &cfg);
	if (rc) {
		os_log(LOG_ERR, "xsk_umem__create() failed with error %d\n", rc);
		goto err_umem_create;
	}

	/* The FILL queue must be a power of 2, but there is no need to fill it up
	 * completely, as long as it always contains buffers when the kernel needs some.
	 */
	rc = xsk_ring_prod__reserve(&umem->fill_ring, FILL_LEVEL_INITIAL, &idx);
	if (rc != FILL_LEVEL_INITIAL) {
		os_log(LOG_ERR, "xsk_ring_prod__reserve() failed with error %d\n", rc);
		goto err_fill_reserve;
	}

	for (i = 0; i < FILL_LEVEL_INITIAL; i++) {
		rc = pool_alloc_shmem(&umem_buffer_pool, (unsigned long *)xsk_ring_prod__fill_addr(&umem->fill_ring, idx + i));
		if (rc) {
			os_log(LOG_ERR, "pool_alloc_shmem() failed with error %d\n", rc);
			goto err_pool_alloc;
		}
	}
	xsk_ring_prod__submit(&umem->fill_ring, FILL_LEVEL_INITIAL);

	pthread_mutex_init(&umem->fill_lock, NULL);
	pthread_mutex_init(&umem->completion_lock, NULL);
	umem->queue_index = queue_index;
	umem->refcnt = 0;

	return umem;


err_pool_alloc:
	while (i > 0) {
		i--;
		pool_free_shmem(&umem_buffer_pool, *xsk_ring_prod__fill_addr(&umem->fill_ring, idx + i));
	}
err_fill_reserve:
	xsk_umem__delete(umem->umem);
err_umem_create:
	free(umem);
err_alloc:
	return NULL;
}

static void net_xdp_umem_delete(struct net_xdp_umem *umem)
{
	net_xdp_umem_completion_cleanup(umem);
	//FIXME Can there be packets in-flight in the driver?

	net_xdp_umem_fill_cleanup(umem);

	xsk_umem__delete(umem->umem);

	free(umem);
}


static struct net_xdp_umem *net_xdp_umem_get(unsigned int queue_index)
{
	struct list_head *entry;
	struct net_xdp_umem *umem = NULL;

	pthread_mutex_lock(&umem_lock);
	for (entry = list_first(&umem_list); entry != &umem_list; entry = list_next(entry)) {
		umem = container_of(entry, struct net_xdp_umem, list);
		if (umem->queue_index == queue_index)
			break;
	}

	if (umem && (umem->queue_index != queue_index))
		umem = NULL;

	if (!umem) {
		umem = net_xdp_umem_create(queue_index);
		if (!umem) {
			os_log(LOG_ERR, "Could not allocate new umem\n");
			goto err;
		}
		list_add(&umem_list, &umem->list);
	}

	umem->refcnt++;

err:
	pthread_mutex_unlock(&umem_lock);

	return umem;
}


static void net_xdp_umem_put_locked(struct net_xdp_umem *umem)
{
	umem->refcnt--;
	if (umem->refcnt == 0) {
		list_del(&umem->list);
		net_xdp_umem_delete(umem);
	}
}

static void net_xdp_umem_put(struct net_xdp_umem *umem)
{
	pthread_mutex_lock(&umem_lock);
	net_xdp_umem_put_locked(umem);
	pthread_mutex_unlock(&umem_lock);
}

static int net_xdp_umem_refill(struct net_xdp_umem *umem)
{
	unsigned int count, i, level;
	uint32_t idx;
	int rc;

	pthread_mutex_lock(&umem->fill_lock);

	/* Try to maintain the FILL queue at a certain level:
	 * if there are already enough buffers, don't add new ones to avoid emptying the pool
	 * if there aren't enough, add some buffers
	 */
	count = xsk_prod_nb_free(&umem->fill_ring, FILL_QUEUE_SIZE);
	level = FILL_QUEUE_SIZE - count;
	count = level < FILL_LEVEL ? FILL_LEVEL - level : 0;

	/* We already computed a safe value for count, so we only call xsk_ring_prod__reserve
	 * to fetch the current index, while preserving internal ring counters for future calls:
	 * in case some allocations fails, we'll be able to reserve and submit just the right amount
	 * afterwards.
	 */
	xsk_ring_prod__reserve(&umem->fill_ring, 0, &idx);

	for (i = 0; i < count; i++) {
		rc = pool_alloc_shmem(&umem_buffer_pool, (unsigned long *)xsk_ring_prod__fill_addr(&umem->fill_ring, idx + i));
		if (rc) {
			os_log(LOG_ERR, "pool_alloc_shmem() failed with error %d\n", rc);
			break;
		}
	}

	xsk_ring_prod__reserve(&umem->fill_ring, i, &idx);
	xsk_ring_prod__submit(&umem->fill_ring, i);

	pthread_mutex_unlock(&umem->fill_lock);

	net_xdp_wakeup(xsk_umem__fd(umem->umem), &umem->fill_ring);

	return i;
}

/*
 * returns 1 if the ptype in the network address is supported, 0 otherwise.
 */
static bool net_address_is_supported(struct net_address *addr)
{
	bool rc;

	switch (addr->ptype) {
	case PTYPE_L2:
		rc = true;
		break;
	default:
		rc = false;
		break;
	}

	return rc;
}

static struct net_xdp_ctx *net_xdp_ctx_init(struct net_address *addr, unsigned int rx_queue_size, unsigned int tx_queue_size)
{
	struct xsk_socket_config cfg = {
			.rx_size = rx_queue_size,
			.tx_size = tx_queue_size,
			.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
			.xdp_flags = XDP_FLAGS_DRV_MODE,
#if !defined(NET_XDP_ZEROCOPY)
			.bind_flags = XDP_USE_NEED_WAKEUP | XDP_COPY,
#else
			.bind_flags = XDP_USE_NEED_WAKEUP | XDP_ZEROCOPY,
#endif
	};
	struct net_xdp_ctx *ctx;
	struct xsk_ring_cons *rx_queue;
	struct xsk_ring_prod *tx_queue;
	unsigned int queue_index;
	int rc;

	if (!addr || !net_address_is_supported(addr))
		goto err_addr;


	queue_index = rx_queue_size? xdp_config.endpoint_queue_rx[addr->port]:xdp_config.endpoint_queue_tx[addr->port];

	ctx = malloc(sizeof(struct net_xdp_ctx));
	if (!ctx) {
		os_log(LOG_ERR, "Could not allocate private context for XDP socket addr %p\n", addr);
		goto err_priv;
	}

	ctx->umem = net_xdp_umem_get(queue_index);
	if (!ctx->umem) {
		os_log(LOG_ERR, "Could not get umem for socket addr %p\n", addr);
		goto err_umem;
	}

	rx_queue = rx_queue_size ? &ctx->rx_queue : NULL;
	tx_queue = tx_queue_size ? &ctx->tx_queue : NULL;

	rc = xsk_socket__create(&ctx->xdpsock, logical_port_name(addr->port), queue_index, ctx->umem->umem, rx_queue, tx_queue, &cfg);
	if (rc != 0) {
		os_log(LOG_ERR, "xsk_socket__create() returned error %s for port(%d) queue(%d)\n", strerror(-rc), addr->port, queue_index);
		goto err_create;
	}

	memcpy(&ctx->addr, addr, sizeof(struct net_address));

	return ctx;

err_create:
	net_xdp_umem_put(ctx->umem);
err_umem:
	free(ctx);
err_priv:
err_addr:
	return NULL;
}

static void net_xdp_ctx_exit(struct net_xdp_ctx *ctx)
{
	xsk_socket__delete(ctx->xdpsock);

	net_xdp_umem_put(ctx->umem);

	free(ctx);
}

struct net_tx_desc *net_xdp_tx_alloc(unsigned int size)
{
	struct net_tx_desc *desc = NULL;

	if (size > DEFAULT_NET_DATA_SIZE)
		return NULL;

	/* First try to recycle a completed buffer */

	/* Then just alloc from the pool */
	desc = pool_alloc(&umem_buffer_pool);
	if (!desc)
		goto exit;

	desc->flags = 0;
	desc->len = 0;
	desc->l2_offset = NET_DATA_OFFSET;

exit:
	return desc;
}

int net_xdp_tx_alloc_multi(struct net_tx_desc **desc, unsigned int n, unsigned int size)
{
	int i;

	for (i = 0; i < n; i++) {
		desc[i] = net_xdp_tx_alloc(size);
		if (!desc[i])
			goto err_malloc;
	}

err_malloc:
	return i;
}

struct net_tx_desc *net_xdp_tx_clone(struct net_tx_desc *src)
{
	struct net_tx_desc *desc = NULL;

	desc = pool_alloc(&umem_buffer_pool);
	if (!desc)
		goto exit;

	memcpy(desc, src, src->l2_offset + src->len);

exit:
	return desc;
}

void net_xdp_tx_free(struct net_tx_desc *buf)
{
	pool_free(&umem_buffer_pool, (void *)pool_align(&umem_buffer_pool, (unsigned long)buf));
}

void net_xdp_rx_free(struct net_rx_desc *buf)
{
	pool_free(&umem_buffer_pool, (void *)pool_align(&umem_buffer_pool, (unsigned long)buf));
}

void net_xdp_free_multi(void **buf, unsigned int n)
{
	int i;

	for (i = 0; i < n; i++)
		pool_free(&umem_buffer_pool, (void *)pool_align(&umem_buffer_pool, (unsigned long)buf[i]));
}

static int __net_xdp_rx_init(struct net_rx *rx, struct net_address *addr, void (*func)(struct net_rx *, struct net_rx_desc *),
		void (*func_multi)(struct net_rx *, struct net_rx_desc **, unsigned int), unsigned int packets, unsigned int latency, int epoll_fd)
{
	struct net_xdp_ctx *ctx;

	ctx = net_xdp_ctx_init(addr, RX_QUEUE_SIZE, 0);
	if (!ctx) {
		os_log(LOG_ERR, "could not create XDP context\n");
		goto err_ctx;
	}

	rx->fd = xsk_socket__fd(ctx->xdpsock);

	if (net_xdp_xskmap_add_addr(addr, rx->fd) < 0) {
		os_log(LOG_ERR, "Could not add addr ptype(%d) for socket(%d) to XSK map\n", addr->ptype, xskmap_fd);
		goto err_xskmap;
	}

	if (epoll_fd >= 0) {
		if (epoll_ctl_add(epoll_fd, rx->fd, EPOLL_TYPE_NET_RX, rx, &rx->epoll_data, EPOLLIN) < 0) {
			os_log(LOG_ERR, "net_rx(%p) epoll_ctl_add() failed\n", rx);
			goto err_epoll_ctl;
		}
	}

	rx->port_id = addr->port;
	rx->priv = ctx;

	rx->func = func;
	rx->func_multi = func_multi;

	os_log(LOG_INIT, "fd(%d)\n", rx->fd);

	return 0;

err_epoll_ctl:
	net_xdp_xskmap_del_addr(addr);
err_xskmap:
	net_xdp_ctx_exit(ctx);
	rx->fd = -1;
err_ctx:
	return -1;
}

int net_xdp_rx_init(struct net_rx *rx, struct net_address *addr, void (*func)(struct net_rx *, struct net_rx_desc *), unsigned long epoll_fd)
{
	return __net_xdp_rx_init(rx, addr, func, NULL, 1, 0, epoll_fd);
}

int net_xdp_rx_init_multi(struct net_rx *rx, struct net_address *addr, void (*func)(struct net_rx *, struct net_rx_desc **, unsigned int), unsigned int packets, unsigned int time, unsigned long epoll_fd)
{
	return __net_xdp_rx_init(rx, addr, NULL, func, packets, time, epoll_fd);
}

void net_xdp_rx_exit(struct net_rx *rx)
{
	struct net_xdp_ctx *ctx = (struct net_xdp_ctx *)rx->priv;

	net_xdp_xskmap_del_addr(&ctx->addr);

	net_xdp_ctx_exit(ctx);

	rx->fd = -1;
	rx->priv = NULL;

	os_log(LOG_INFO, "done\n");
}

static inline struct net_rx_desc *data_start_to_rx_desc(uint64_t start)
{
	struct net_rx_desc *desc = (struct net_rx_desc *)(start - NET_DATA_OFFSET);
	desc->l2_offset = NET_DATA_OFFSET;

	return desc;
}

struct net_rx_desc *__net_xdp_rx(struct net_rx *rx)
{
	struct net_xdp_ctx *ctx = (struct net_xdp_ctx *)rx->priv;
	struct net_rx_desc *desc = NULL;
	unsigned int count;
	uint32_t idx, len;
	uint64_t addr;

	count = xsk_ring_cons__peek(&ctx->rx_queue, 1, &idx);
	if (count == 0) {
		os_log(LOG_ERR, "Rx ring empty for rx(%p) queue(%d)\n", rx, ctx->umem->queue_index);
		goto err;
	}

	addr = xsk_ring_cons__rx_desc(&ctx->rx_queue, idx)->addr;
	len = xsk_ring_cons__rx_desc(&ctx->rx_queue, idx)->len;

	addr = xsk_umem__add_offset_to_addr(addr);
	addr = (uint64_t) xsk_umem__get_data(umem_buffer_pool_area, addr);
	desc = data_start_to_rx_desc(addr);

	desc->len = len;
	desc->port = rx->port_id;

	net_std_rx_parser(rx, desc);

	xsk_ring_cons__release(&ctx->rx_queue, 1);

	net_xdp_umem_refill(ctx->umem);

	return desc;

err:
	return NULL;
}

void net_xdp_rx_multi(struct net_rx *rx)
{
	struct net_rx_desc *desc[NET_RX_BATCH];
	int i = 0;

	do {
		desc[i] = __net_xdp_rx(rx);
		if (!desc[i])
			break;
		i++;
	} while ( i < NET_RX_BATCH);

	rx->func_multi(rx, desc, i);
}

void net_xdp_rx(struct net_rx *rx)
{
	struct net_rx_desc *desc;

	desc = __net_xdp_rx(rx);
	if (desc)
		rx->func(rx, desc);
}

int net_xdp_tx_init(struct net_tx *tx, struct net_address *addr)
{
	struct net_xdp_ctx *ctx;

	ctx = net_xdp_ctx_init(addr, 0, TX_QUEUE_SIZE);
	if (!ctx) {
		os_log(LOG_ERR, "could not create XDP context\n");
		goto err_ctx;
	}

	tx->priv = ctx;
	tx->port_id = addr->port;
	tx->fd = xsk_socket__fd(ctx->xdpsock);

	os_log(LOG_INIT, "fd(%d)\n", tx->fd);

	return 0;

err_ctx:
	return -1;
}

void net_xdp_tx_exit(struct net_tx *tx)
{
	struct net_xdp_ctx *ctx = (struct net_xdp_ctx *)tx->priv;

	net_xdp_ctx_exit(ctx);

	tx->fd = -1;
	tx->priv = NULL;

	os_log(LOG_INFO, "done\n");
}

int net_xdp_tx(struct net_tx *tx, struct net_tx_desc *desc)
{
	struct net_xdp_ctx *ctx = (struct net_xdp_ctx *)tx->priv;
	struct xdp_desc *tx_desc;
	uint32_t idx;
	unsigned int count;

	count = xsk_ring_prod__reserve(&ctx->tx_queue, 1, &idx);
	if (count == 0) {
		os_log(LOG_ERR, "Tx ring full for tx(%p) queue(%d)\n", tx, ctx->umem->queue_index);
		goto err;
	}

	tx_desc = xsk_ring_prod__tx_desc(&ctx->tx_queue, idx);
	tx_desc->addr = pool_virt_to_shmem(&umem_buffer_pool, NET_DATA_START(desc));
	tx_desc->len = desc->len;
	xsk_ring_prod__submit(&ctx->tx_queue, 1);

	net_xdp_wakeup(tx->fd, &ctx->tx_queue);

	net_xdp_umem_completion_cleanup(ctx->umem);

	return 1;
err:
	return -1;
}

int net_xdp_tx_multi(struct net_tx *tx, struct net_tx_desc **desc, unsigned int n)
{
	unsigned int written = 0;
	int i, rc;

	while (written < n) {
		rc = net_xdp_tx(tx, desc[written]);
		if (rc < 0)
			goto err;

		written++;
	}

err:
	for (i = written; i < n; i++)
		net_xdp_tx_free(desc[i]);

	if (written)
		return written;
	else
		return -1;
}

int net_xdp_tx_ts_get(struct net_tx *tx, uint64_t *ts, unsigned int *private)
{
	return -1;
}

int net_xdp_tx_ts_init(struct net_tx *tx, struct net_address *addr, void (*func)(struct net_tx *, uint64_t, unsigned int), unsigned long priv)
{
	return -1;
}

int net_xdp_tx_ts_exit(struct net_tx *tx)
{
	return 0;
}

unsigned int net_xdp_tx_available(struct net_tx *tx)
{
	struct net_xdp_ctx *ctx = (struct net_xdp_ctx *)tx->priv;

	return xsk_prod_nb_free(&ctx->tx_queue, TX_QUEUE_SIZE);
}

int net_xdp_port_sr_config(unsigned int port_id, uint8_t *sr_class) {
	return 0;
}

void net_xdp_exit(void)
{
	struct list_head *entry;
	struct net_xdp_umem *umem;

	pthread_mutex_lock(&umem_lock);
	entry = list_first(&umem_list);
	while (entry != &umem_list) {
		umem = container_of(entry, struct net_xdp_umem, list);
		entry = list_next(entry);
		net_xdp_umem_put(umem);
	}
	pthread_mutex_unlock(&umem_lock);

	pool_exit(&umem_buffer_pool);
	munmap(umem_buffer_pool_area, BUF_POOL_SIZE);
}

const static struct net_ops_cb net_xdp_ops = {
		.net_exit = net_xdp_exit,
		.net_rx_init = net_xdp_rx_init,
		.net_rx_init_multi = net_xdp_rx_init_multi,
		.net_rx_exit = net_xdp_rx_exit,
		.__net_rx = __net_xdp_rx,
		.net_rx = net_xdp_rx,
		.net_rx_multi = net_xdp_rx_multi,

		.net_tx_init = net_xdp_tx_init,
		.net_tx_exit = net_xdp_tx_exit,
		.net_tx = net_xdp_tx,
		.net_tx_multi = net_xdp_tx_multi,

		.net_tx_ts_get = net_xdp_tx_ts_get,
		.net_tx_ts_init = net_xdp_tx_ts_init,
		.net_tx_ts_exit = net_xdp_tx_ts_exit,

		.net_add_multi = net_std_add_multi,
		.net_del_multi = net_std_del_multi,

		.net_tx_alloc = net_xdp_tx_alloc,
		.net_tx_alloc_multi = net_xdp_tx_alloc_multi,
		.net_tx_clone = net_xdp_tx_clone,
		.net_tx_free = net_xdp_tx_free,
		.net_rx_free = net_xdp_rx_free,
		.net_free_multi = net_xdp_free_multi,

		.net_tx_available = net_xdp_tx_available,
		.net_port_status = net_dflt_port_status,
		.net_port_sr_config = net_xdp_port_sr_config,
};

int net_xdp_init(struct net_ops_cb *net_ops, struct os_xdp_config *config)
{
	int rc;

	list_head_init(&umem_list);
	pthread_mutex_init(&umem_lock, NULL);

	umem_buffer_pool_area = mmap(NULL, BUF_POOL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (umem_buffer_pool_area == MAP_FAILED) {
		os_log(LOG_ERR, "mmap() failed: %s\n", strerror(errno));
		goto err;
	}

	rc = pool_init(&umem_buffer_pool, umem_buffer_pool_area, BUF_POOL_SIZE, BUF_ORDER);
	if (rc) {
		os_log(LOG_ERR, "pool_init() failed with error %d\n", rc);
		goto err_pool;
	}

	xdpkey_fd = bpf_obj_get(GENAVB_XDPKEY_NAME);
	if (xdpkey_fd == -1) {
		os_log(LOG_ERR, "Could not find XSK map\n");
		goto err_maps;
	} else {
		xskmap_fd = bpf_obj_get(GENAVB_XSKMAP_NAME);
		if (xskmap_fd == -1) {
			xdpkey_fd = -1;
			os_log(LOG_ERR, "Could not find XSK map\n");
			goto err_maps;
		}
	}
	memset(free_index_bmap, 0xff, sizeof(free_index_bmap));

	/* We copy the entire struct rather than just point to it, to reduce the number of
	 * indirections in performance-sensitive code.
	 */
	os_memcpy(net_ops, &net_xdp_ops, sizeof(struct net_ops_cb));
	os_memcpy(&xdp_config, config, sizeof(struct os_xdp_config));

	os_log(LOG_INIT, "done, using rx_queue(%d) and tx_queue(%d)\n", xdp_config.endpoint_queue_rx[0], xdp_config.endpoint_queue_tx[0]);

	return 0;

err_maps:
	pool_exit(&umem_buffer_pool);
err_pool:
	munmap(umem_buffer_pool_area, BUF_POOL_SIZE);
err:
	return -1;
}


