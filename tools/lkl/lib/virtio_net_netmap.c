#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <poll.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <assert.h>

#include <sys/mman.h>
#ifdef CONFIG_AUTO_LKL_VIRTIO_NET_NETMAP
#include <net/netmap.h>
#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

#include "virtio.h"

#include <lkl_host.h>

static int vale_rings = 0;

#define BURST_MAX 1024
#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))

struct lkl_netdev_netmap {
	struct lkl_netdev dev;
	int fd;
	char *mem;
	struct nmreq nmr;
	struct netmap_if *nifp;
	int close;
	int nr_tx;
	int nr_rx;
};

static int net_tx_end(struct lkl_netdev *nd)
{
	struct lkl_netdev_netmap *nd_netmap;
	int ret = 0;

	nd_netmap = (struct lkl_netdev_netmap *) nd;
	if (nd_netmap->nr_tx) {
		nd_netmap->nr_tx = 0;
//		printf("tx_end\n");
		ret = ioctl(nd_netmap->fd, NIOCTXSYNC, NULL);
	}
	return ret;
}

static int net_rx_end(struct lkl_netdev *nd)
{
	struct lkl_netdev_netmap *nd_netmap;
	int ret = 0;

	nd_netmap = (struct lkl_netdev_netmap *) nd;
	if (nd_netmap->nr_rx) {
		nd_netmap->nr_rx = 0;
//		printf("rx_end\n");
		ret = ioctl(nd_netmap->fd, NIOCRXSYNC, NULL);
	}
	return ret;
}

static int net_tx(struct lkl_netdev *nd, struct iovec *iov, int cnt)
{
	struct lkl_netdev_netmap *nd_netmap;
	int i, len;
	uint32_t cur;
	struct netmap_slot *ts;
	struct netmap_ring *ring;

	nd_netmap = (struct lkl_netdev_netmap *)nd;
	ring = NETMAP_TXRING(nd_netmap->nifp, 0);

	if (nm_ring_empty(ring)) {
		net_tx_end(nd);
		if (nm_ring_empty(ring)) {
			return 0;
		}
	}

	cur = ring->cur;
	ts = &ring->slot[cur];
	len = 0;
	for (i=0; i<cnt; i++) {
		memcpy(NETMAP_BUF(ring, ts->buf_idx)+len, iov[i].iov_base, iov[i].iov_len);
		len += iov[i].iov_len;
	}
	ts->len = len;

	cur = nm_ring_next(ring, cur);
	ring->head = ring->cur = cur;
	if (len)
		++nd_netmap->nr_tx;

	return len ?: -1;
}

/*
 * this function is not thread-safe.
 *
 * nd_netmap->rms is specifically not safe in parallel access.  if future
 * refactor allows us to read in parallel, the buffer (nd_netmap->rms) shall
 * be guarded.
 */
static int net_rx(struct lkl_netdev *nd, struct iovec *iov, int cnt)
{
	struct lkl_netdev_netmap *nd_netmap;
	int seg, copied, size;
	uint32_t cur, n;
	struct netmap_slot *rs;
	struct netmap_ring *ring;
	struct netmap_if *nifp;

	nd_netmap = (struct lkl_netdev_netmap *) nd;
	nifp = nd_netmap->nifp;
	copied = 0;
	for (n = 0; n < nifp->ni_rx_rings; n++) {
		char *p;

		ring = NETMAP_RXRING(nifp, n);

		if (nm_ring_empty(ring)) {
			net_rx_end(nd);
			if (nm_ring_empty(ring))
				continue;
		}

//		uint32_t m, rx;
//		m = nm_ring_space(ring);
//		m = MIN(m, BURST_MAX);

		cur = ring->cur;
//		for (rx = 0; rx < m; rx++) {
			rs = &ring->slot[cur];
			size = rs->len;
			p = NETMAP_BUF(ring, rs->buf_idx);

			for (seg = 0; copied < size && seg < cnt; seg++) {
				uint32_t to_copy;

				to_copy = size - copied;
				if (to_copy > iov[seg].iov_len)
					to_copy = iov[seg].iov_len;
				memcpy(iov[seg].iov_base, p+copied, to_copy);
				copied += to_copy;
			}
			cur = nm_ring_next(ring, cur);
//		}
		ring->head = ring->cur = cur;
		if (copied)
			++nd_netmap->nr_rx;
	}

	return copied ?: -1;
}

static int net_poll(struct lkl_netdev *nd)
{
	struct lkl_netdev_netmap *nd_netmap =
		container_of(nd, struct lkl_netdev_netmap, dev);
	struct pollfd x;
	int ret;

	x.fd = nd_netmap->fd;
	x.events = POLLIN | POLLHUP | POLLERR; // | POLLOUT;
	ret = poll(&x, 1, -1);
	if (ret < 0)
		return -1;

	ret = LKL_DEV_NET_POLL_TX;
	if (x.revents & POLLIN) {
		ret |= LKL_DEV_NET_POLL_RX;
	}
//	if (x.revents & POLLOUT) {
//		ret |= LKL_DEV_NET_POLL_TX;
//	}
	if (x.revents & (POLLHUP|POLLERR)) {
		ret |= LKL_DEV_NET_POLL_HUP;
	}
	if (nd_netmap->close) {
		ret |= LKL_DEV_NET_POLL_HUP;
	}

	return ret;
}

static void net_poll_hup(struct lkl_netdev *nd)
{
	struct lkl_netdev_netmap *nd_netmap =
		container_of(nd, struct lkl_netdev_netmap, dev);

	close(nd_netmap->fd);
	nd_netmap->close = 1;
}

static void net_free(struct lkl_netdev *nd)
{
	struct lkl_netdev_netmap *nd_netmap =
		container_of(nd, struct lkl_netdev_netmap, dev);

	munmap(nd_netmap->mem, nd_netmap->nmr.nr_memsize);
	if (!nd_netmap->close)
		close(nd_netmap->fd);
	free(nd_netmap);
}

struct lkl_dev_net_ops netmap_net_ops = {
	.tx = net_tx,
	.tx_end = net_tx_end,
	.rx = net_rx,
	.rx_end = net_rx_end,
	.poll = net_poll,
	.poll_hup = net_poll_hup,
	.free = net_free,
};

struct lkl_netdev *lkl_netdev_netmap_create(const char *ifname)
{
	int fd = 0;
	struct lkl_netdev_netmap *nd;

	fd = open("/dev/netmap", O_RDWR);
	if (fd < 0) {
		printf("unable to open /dev/netmap\n");
		return NULL;
	}

	nd = malloc(sizeof(struct lkl_netdev_netmap));
	if (!nd) {
		printf("unable to register interface %s: out of memory\n", ifname);
		close(fd);
		return NULL;
	}
	memset(nd, 0, sizeof(struct lkl_netdev_netmap));
	nd->fd = fd;
	nd->dev.ops = &netmap_net_ops;

	strcpy(nd->nmr.nr_name, ifname);
	nd->nmr.nr_version = NETMAP_API;
	nd->nmr.nr_ringid = 0; // | (NETMAP_NO_TX_POLL | NETMAP_NO_RX_POLL);
	nd->nmr.nr_flags |= NR_REG_ALL_NIC;

	if (ioctl(fd, NIOCREGIF, &nd->nmr) < 0) {
		printf("unable to register interface %s: NIOCREGIF\n", ifname);
		close(fd);
		free(nd);
		return NULL;
	}

	if (vale_rings && strncmp(ifname, "vale", 4) == 0) {
		nd->nmr.nr_rx_rings = vale_rings;
		nd->nmr.nr_tx_rings = vale_rings;
	}

	nd->mem = mmap(NULL, nd->nmr.nr_memsize,
		   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (nd->mem == MAP_FAILED) {
		printf("unable to register interface %s: mmap\n", ifname);
		close(fd);
		free(nd);
		return NULL;
	}
	nd->nifp = NETMAP_IF(nd->mem, nd->nmr.nr_offset);

	return (struct lkl_netdev *) nd;
}
#endif
