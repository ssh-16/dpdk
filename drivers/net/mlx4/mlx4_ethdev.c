/*-
 *   BSD LICENSE
 *
 *   Copyright 2017 6WIND S.A.
 *   Copyright 2017 Mellanox
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
 *     * Neither the name of 6WIND S.A. nor the names of its
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

/**
 * @file
 * Miscellaneous control operations for mlx4 driver.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* Verbs headers do not support -pedantic. */
#ifdef PEDANTIC
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <infiniband/verbs.h>
#ifdef PEDANTIC
#pragma GCC diagnostic error "-Wpedantic"
#endif

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_pci.h>

#include "mlx4.h"
#include "mlx4_rxtx.h"
#include "mlx4_utils.h"

/**
 * Get interface name from private structure.
 *
 * @param[in] priv
 *   Pointer to private structure.
 * @param[out] ifname
 *   Interface name output buffer.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
int
mlx4_get_ifname(const struct priv *priv, char (*ifname)[IF_NAMESIZE])
{
	DIR *dir;
	struct dirent *dent;
	unsigned int dev_type = 0;
	unsigned int dev_port_prev = ~0u;
	char match[IF_NAMESIZE] = "";

	{
		MKSTR(path, "%s/device/net", priv->ctx->device->ibdev_path);

		dir = opendir(path);
		if (dir == NULL) {
			rte_errno = errno;
			return -rte_errno;
		}
	}
	while ((dent = readdir(dir)) != NULL) {
		char *name = dent->d_name;
		FILE *file;
		unsigned int dev_port;
		int r;

		if ((name[0] == '.') &&
		    ((name[1] == '\0') ||
		     ((name[1] == '.') && (name[2] == '\0'))))
			continue;

		MKSTR(path, "%s/device/net/%s/%s",
		      priv->ctx->device->ibdev_path, name,
		      (dev_type ? "dev_id" : "dev_port"));

		file = fopen(path, "rb");
		if (file == NULL) {
			if (errno != ENOENT)
				continue;
			/*
			 * Switch to dev_id when dev_port does not exist as
			 * is the case with Linux kernel versions < 3.15.
			 */
try_dev_id:
			match[0] = '\0';
			if (dev_type)
				break;
			dev_type = 1;
			dev_port_prev = ~0u;
			rewinddir(dir);
			continue;
		}
		r = fscanf(file, (dev_type ? "%x" : "%u"), &dev_port);
		fclose(file);
		if (r != 1)
			continue;
		/*
		 * Switch to dev_id when dev_port returns the same value for
		 * all ports. May happen when using a MOFED release older than
		 * 3.0 with a Linux kernel >= 3.15.
		 */
		if (dev_port == dev_port_prev)
			goto try_dev_id;
		dev_port_prev = dev_port;
		if (dev_port == (priv->port - 1u))
			snprintf(match, sizeof(match), "%s", name);
	}
	closedir(dir);
	if (match[0] == '\0') {
		rte_errno = ENODEV;
		return -rte_errno;
	}
	strncpy(*ifname, match, sizeof(*ifname));
	return 0;
}

/**
 * Read from sysfs entry.
 *
 * @param[in] priv
 *   Pointer to private structure.
 * @param[in] entry
 *   Entry name relative to sysfs path.
 * @param[out] buf
 *   Data output buffer.
 * @param size
 *   Buffer size.
 *
 * @return
 *   Number of bytes read on success, negative errno value otherwise and
 *   rte_errno is set.
 */
static int
mlx4_sysfs_read(const struct priv *priv, const char *entry,
		char *buf, size_t size)
{
	char ifname[IF_NAMESIZE];
	FILE *file;
	int ret;

	ret = mlx4_get_ifname(priv, &ifname);
	if (ret)
		return ret;

	MKSTR(path, "%s/device/net/%s/%s", priv->ctx->device->ibdev_path,
	      ifname, entry);

	file = fopen(path, "rb");
	if (file == NULL) {
		rte_errno = errno;
		return -rte_errno;
	}
	ret = fread(buf, 1, size, file);
	if ((size_t)ret < size && ferror(file)) {
		rte_errno = EIO;
		ret = -rte_errno;
	} else {
		ret = size;
	}
	fclose(file);
	return ret;
}

/**
 * Write to sysfs entry.
 *
 * @param[in] priv
 *   Pointer to private structure.
 * @param[in] entry
 *   Entry name relative to sysfs path.
 * @param[in] buf
 *   Data buffer.
 * @param size
 *   Buffer size.
 *
 * @return
 *   Number of bytes written on success, negative errno value otherwise and
 *   rte_errno is set.
 */
static int
mlx4_sysfs_write(const struct priv *priv, const char *entry,
		 char *buf, size_t size)
{
	char ifname[IF_NAMESIZE];
	FILE *file;
	int ret;

	ret = mlx4_get_ifname(priv, &ifname);
	if (ret)
		return ret;

	MKSTR(path, "%s/device/net/%s/%s", priv->ctx->device->ibdev_path,
	      ifname, entry);

	file = fopen(path, "wb");
	if (file == NULL) {
		rte_errno = errno;
		return -rte_errno;
	}
	ret = fwrite(buf, 1, size, file);
	if ((size_t)ret < size || ferror(file)) {
		rte_errno = EIO;
		ret = -rte_errno;
	} else {
		ret = size;
	}
	fclose(file);
	return ret;
}

/**
 * Get unsigned long sysfs property.
 *
 * @param priv
 *   Pointer to private structure.
 * @param[in] name
 *   Entry name relative to sysfs path.
 * @param[out] value
 *   Value output buffer.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
static int
mlx4_get_sysfs_ulong(struct priv *priv, const char *name, unsigned long *value)
{
	int ret;
	unsigned long value_ret;
	char value_str[32];

	ret = mlx4_sysfs_read(priv, name, value_str, (sizeof(value_str) - 1));
	if (ret < 0) {
		DEBUG("cannot read %s value from sysfs: %s",
		      name, strerror(rte_errno));
		return ret;
	}
	value_str[ret] = '\0';
	errno = 0;
	value_ret = strtoul(value_str, NULL, 0);
	if (errno) {
		rte_errno = errno;
		DEBUG("invalid %s value `%s': %s", name, value_str,
		      strerror(rte_errno));
		return -rte_errno;
	}
	*value = value_ret;
	return 0;
}

/**
 * Set unsigned long sysfs property.
 *
 * @param priv
 *   Pointer to private structure.
 * @param[in] name
 *   Entry name relative to sysfs path.
 * @param value
 *   Value to set.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
static int
mlx4_set_sysfs_ulong(struct priv *priv, const char *name, unsigned long value)
{
	int ret;
	MKSTR(value_str, "%lu", value);

	ret = mlx4_sysfs_write(priv, name, value_str, (sizeof(value_str) - 1));
	if (ret < 0) {
		DEBUG("cannot write %s `%s' (%lu) to sysfs: %s",
		      name, value_str, value, strerror(rte_errno));
		return ret;
	}
	return 0;
}

/**
 * Perform ifreq ioctl() on associated Ethernet device.
 *
 * @param[in] priv
 *   Pointer to private structure.
 * @param req
 *   Request number to pass to ioctl().
 * @param[out] ifr
 *   Interface request structure output buffer.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
static int
mlx4_ifreq(const struct priv *priv, int req, struct ifreq *ifr)
{
	int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
	int ret;

	if (sock == -1) {
		rte_errno = errno;
		return -rte_errno;
	}
	ret = mlx4_get_ifname(priv, &ifr->ifr_name);
	if (!ret && ioctl(sock, req, ifr) == -1) {
		rte_errno = errno;
		ret = -rte_errno;
	}
	close(sock);
	return ret;
}

/**
 * Get MAC address by querying netdevice.
 *
 * @param[in] priv
 *   Pointer to private structure.
 * @param[out] mac
 *   MAC address output buffer.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
int
mlx4_get_mac(struct priv *priv, uint8_t (*mac)[ETHER_ADDR_LEN])
{
	struct ifreq request;
	int ret = mlx4_ifreq(priv, SIOCGIFHWADDR, &request);

	if (ret)
		return ret;
	memcpy(mac, request.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);
	return 0;
}

/**
 * Get device MTU.
 *
 * @param priv
 *   Pointer to private structure.
 * @param[out] mtu
 *   MTU value output buffer.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
int
mlx4_mtu_get(struct priv *priv, uint16_t *mtu)
{
	unsigned long ulong_mtu = 0;
	int ret = mlx4_get_sysfs_ulong(priv, "mtu", &ulong_mtu);

	if (ret)
		return ret;
	*mtu = ulong_mtu;
	return 0;
}

/**
 * DPDK callback to change the MTU.
 *
 * @param priv
 *   Pointer to Ethernet device structure.
 * @param mtu
 *   MTU value to set.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
int
mlx4_mtu_set(struct rte_eth_dev *dev, uint16_t mtu)
{
	struct priv *priv = dev->data->dev_private;
	uint16_t new_mtu;
	int ret = mlx4_set_sysfs_ulong(priv, "mtu", mtu);

	if (ret)
		return ret;
	ret = mlx4_mtu_get(priv, &new_mtu);
	if (ret)
		return ret;
	if (new_mtu == mtu) {
		priv->mtu = mtu;
		return 0;
	}
	rte_errno = EINVAL;
	return -rte_errno;
}

/**
 * Set device flags.
 *
 * @param priv
 *   Pointer to private structure.
 * @param keep
 *   Bitmask for flags that must remain untouched.
 * @param flags
 *   Bitmask for flags to modify.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
static int
mlx4_set_flags(struct priv *priv, unsigned int keep, unsigned int flags)
{
	unsigned long tmp = 0;
	int ret = mlx4_get_sysfs_ulong(priv, "flags", &tmp);

	if (ret)
		return ret;
	tmp &= keep;
	tmp |= (flags & (~keep));
	return mlx4_set_sysfs_ulong(priv, "flags", tmp);
}

/**
 * Change the link state (UP / DOWN).
 *
 * @param priv
 *   Pointer to Ethernet device private data.
 * @param up
 *   Nonzero for link up, otherwise link down.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
static int
mlx4_dev_set_link(struct priv *priv, int up)
{
	struct rte_eth_dev *dev = priv->dev;
	int err;

	if (up) {
		err = mlx4_set_flags(priv, ~IFF_UP, IFF_UP);
		if (err)
			return err;
		dev->rx_pkt_burst = mlx4_rx_burst;
	} else {
		err = mlx4_set_flags(priv, ~IFF_UP, ~IFF_UP);
		if (err)
			return err;
		dev->rx_pkt_burst = mlx4_rx_burst_removed;
		dev->tx_pkt_burst = mlx4_tx_burst_removed;
	}
	return 0;
}

/**
 * DPDK callback to bring the link DOWN.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
int
mlx4_dev_set_link_down(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;

	return mlx4_dev_set_link(priv, 0);
}

/**
 * DPDK callback to bring the link UP.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
int
mlx4_dev_set_link_up(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;

	return mlx4_dev_set_link(priv, 1);
}

/**
 * DPDK callback to get information about the device.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] info
 *   Info structure output buffer.
 */
void
mlx4_dev_infos_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *info)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int max;
	char ifname[IF_NAMESIZE];

	info->pci_dev = RTE_ETH_DEV_TO_PCI(dev);
	if (priv == NULL)
		return;
	/* FIXME: we should ask the device for these values. */
	info->min_rx_bufsize = 32;
	info->max_rx_pktlen = 65536;
	/*
	 * Since we need one CQ per QP, the limit is the minimum number
	 * between the two values.
	 */
	max = ((priv->device_attr.max_cq > priv->device_attr.max_qp) ?
	       priv->device_attr.max_qp : priv->device_attr.max_cq);
	/* If max >= 65535 then max = 0, max_rx_queues is uint16_t. */
	if (max >= 65535)
		max = 65535;
	info->max_rx_queues = max;
	info->max_tx_queues = max;
	/* Last array entry is reserved for broadcast. */
	info->max_mac_addrs = 1;
	info->rx_offload_capa = 0;
	info->tx_offload_capa = 0;
	if (mlx4_get_ifname(priv, &ifname) == 0)
		info->if_index = if_nametoindex(ifname);
	info->speed_capa =
			ETH_LINK_SPEED_1G |
			ETH_LINK_SPEED_10G |
			ETH_LINK_SPEED_20G |
			ETH_LINK_SPEED_40G |
			ETH_LINK_SPEED_56G;
}

/**
 * DPDK callback to get device statistics.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] stats
 *   Stats structure output buffer.
 */
void
mlx4_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct priv *priv = dev->data->dev_private;
	struct rte_eth_stats tmp;
	unsigned int i;
	unsigned int idx;

	if (priv == NULL)
		return;
	memset(&tmp, 0, sizeof(tmp));
	/* Add software counters. */
	for (i = 0; (i != priv->rxqs_n); ++i) {
		struct rxq *rxq = (*priv->rxqs)[i];

		if (rxq == NULL)
			continue;
		idx = rxq->stats.idx;
		if (idx < RTE_ETHDEV_QUEUE_STAT_CNTRS) {
			tmp.q_ipackets[idx] += rxq->stats.ipackets;
			tmp.q_ibytes[idx] += rxq->stats.ibytes;
			tmp.q_errors[idx] += (rxq->stats.idropped +
					      rxq->stats.rx_nombuf);
		}
		tmp.ipackets += rxq->stats.ipackets;
		tmp.ibytes += rxq->stats.ibytes;
		tmp.ierrors += rxq->stats.idropped;
		tmp.rx_nombuf += rxq->stats.rx_nombuf;
	}
	for (i = 0; (i != priv->txqs_n); ++i) {
		struct txq *txq = (*priv->txqs)[i];

		if (txq == NULL)
			continue;
		idx = txq->stats.idx;
		if (idx < RTE_ETHDEV_QUEUE_STAT_CNTRS) {
			tmp.q_opackets[idx] += txq->stats.opackets;
			tmp.q_obytes[idx] += txq->stats.obytes;
			tmp.q_errors[idx] += txq->stats.odropped;
		}
		tmp.opackets += txq->stats.opackets;
		tmp.obytes += txq->stats.obytes;
		tmp.oerrors += txq->stats.odropped;
	}
	*stats = tmp;
}

/**
 * DPDK callback to clear device statistics.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
void
mlx4_stats_reset(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;
	unsigned int idx;

	if (priv == NULL)
		return;
	for (i = 0; (i != priv->rxqs_n); ++i) {
		if ((*priv->rxqs)[i] == NULL)
			continue;
		idx = (*priv->rxqs)[i]->stats.idx;
		(*priv->rxqs)[i]->stats =
			(struct mlx4_rxq_stats){ .idx = idx };
	}
	for (i = 0; (i != priv->txqs_n); ++i) {
		if ((*priv->txqs)[i] == NULL)
			continue;
		idx = (*priv->txqs)[i]->stats.idx;
		(*priv->txqs)[i]->stats =
			(struct mlx4_txq_stats){ .idx = idx };
	}
}

/**
 * DPDK callback to retrieve physical link information.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param wait_to_complete
 *   Wait for request completion (ignored).
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
int
mlx4_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	const struct priv *priv = dev->data->dev_private;
	struct ethtool_cmd edata = {
		.cmd = ETHTOOL_GSET,
	};
	struct ifreq ifr;
	struct rte_eth_link dev_link;
	int link_speed = 0;

	if (priv == NULL) {
		rte_errno = EINVAL;
		return -rte_errno;
	}
	(void)wait_to_complete;
	if (mlx4_ifreq(priv, SIOCGIFFLAGS, &ifr)) {
		WARN("ioctl(SIOCGIFFLAGS) failed: %s", strerror(rte_errno));
		return -rte_errno;
	}
	memset(&dev_link, 0, sizeof(dev_link));
	dev_link.link_status = ((ifr.ifr_flags & IFF_UP) &&
				(ifr.ifr_flags & IFF_RUNNING));
	ifr.ifr_data = (void *)&edata;
	if (mlx4_ifreq(priv, SIOCETHTOOL, &ifr)) {
		WARN("ioctl(SIOCETHTOOL, ETHTOOL_GSET) failed: %s",
		     strerror(rte_errno));
		return -rte_errno;
	}
	link_speed = ethtool_cmd_speed(&edata);
	if (link_speed == -1)
		dev_link.link_speed = 0;
	else
		dev_link.link_speed = link_speed;
	dev_link.link_duplex = ((edata.duplex == DUPLEX_HALF) ?
				ETH_LINK_HALF_DUPLEX : ETH_LINK_FULL_DUPLEX);
	dev_link.link_autoneg = !(dev->data->dev_conf.link_speeds &
				  ETH_LINK_SPEED_FIXED);
	dev->data->dev_link = dev_link;
	return 0;
}

/**
 * DPDK callback to get flow control status.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[out] fc_conf
 *   Flow control output buffer.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
int
mlx4_flow_ctrl_get(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct priv *priv = dev->data->dev_private;
	struct ifreq ifr;
	struct ethtool_pauseparam ethpause = {
		.cmd = ETHTOOL_GPAUSEPARAM,
	};
	int ret;

	ifr.ifr_data = (void *)&ethpause;
	if (mlx4_ifreq(priv, SIOCETHTOOL, &ifr)) {
		ret = rte_errno;
		WARN("ioctl(SIOCETHTOOL, ETHTOOL_GPAUSEPARAM)"
		     " failed: %s",
		     strerror(rte_errno));
		goto out;
	}
	fc_conf->autoneg = ethpause.autoneg;
	if (ethpause.rx_pause && ethpause.tx_pause)
		fc_conf->mode = RTE_FC_FULL;
	else if (ethpause.rx_pause)
		fc_conf->mode = RTE_FC_RX_PAUSE;
	else if (ethpause.tx_pause)
		fc_conf->mode = RTE_FC_TX_PAUSE;
	else
		fc_conf->mode = RTE_FC_NONE;
	ret = 0;
out:
	assert(ret >= 0);
	return -ret;
}

/**
 * DPDK callback to modify flow control parameters.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 * @param[in] fc_conf
 *   Flow control parameters.
 *
 * @return
 *   0 on success, negative errno value otherwise and rte_errno is set.
 */
int
mlx4_flow_ctrl_set(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct priv *priv = dev->data->dev_private;
	struct ifreq ifr;
	struct ethtool_pauseparam ethpause = {
		.cmd = ETHTOOL_SPAUSEPARAM,
	};
	int ret;

	ifr.ifr_data = (void *)&ethpause;
	ethpause.autoneg = fc_conf->autoneg;
	if (((fc_conf->mode & RTE_FC_FULL) == RTE_FC_FULL) ||
	    (fc_conf->mode & RTE_FC_RX_PAUSE))
		ethpause.rx_pause = 1;
	else
		ethpause.rx_pause = 0;
	if (((fc_conf->mode & RTE_FC_FULL) == RTE_FC_FULL) ||
	    (fc_conf->mode & RTE_FC_TX_PAUSE))
		ethpause.tx_pause = 1;
	else
		ethpause.tx_pause = 0;
	if (mlx4_ifreq(priv, SIOCETHTOOL, &ifr)) {
		ret = rte_errno;
		WARN("ioctl(SIOCETHTOOL, ETHTOOL_SPAUSEPARAM)"
		     " failed: %s",
		     strerror(rte_errno));
		goto out;
	}
	ret = 0;
out:
	assert(ret >= 0);
	return -ret;
}