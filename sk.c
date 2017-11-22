/**
 * @file sk.c
 * @brief Implements protocol independent socket methods.
 * @note Copyright (C) 2012 Richard Cochran <richardcochran@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <errno.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <stdlib.h>
#include <poll.h>

#include "address.h"
#include "ether.h"
#include "missing.h"
#include "print.h"
#include "sk.h"
#ifdef SJA1105_TC
#include "sja1105-ptp.h"
#include "msg.h"
#endif

/* globals */

#ifdef SJA1105_TC
int sk_meta_timeout = 1;
#endif
int sk_tx_timeout = 1;
int sk_check_fupsync;

/* private methods */

static int hwts_init(int fd, const char *device, int rx_filter, int one_step)
{
	struct ifreq ifreq;
	struct hwtstamp_config cfg, req;
	int err;

	memset(&ifreq, 0, sizeof(ifreq));
	memset(&cfg, 0, sizeof(cfg));

	strncpy(ifreq.ifr_name, device, sizeof(ifreq.ifr_name) - 1);

	ifreq.ifr_data = (void *) &cfg;
	cfg.tx_type    = one_step ? HWTSTAMP_TX_ONESTEP_SYNC : HWTSTAMP_TX_ON;
	cfg.rx_filter  = rx_filter;
	req = cfg;
	err = ioctl(fd, SIOCSHWTSTAMP, &ifreq);
	if (err < 0)
		return err;

	if (memcmp(&cfg, &req, sizeof(cfg))) {

		pr_warning("driver changed our HWTSTAMP options");
		pr_warning("tx_type   %d not %d", cfg.tx_type, req.tx_type);
		pr_warning("rx_filter %d not %d", cfg.rx_filter, req.rx_filter);

		if (cfg.tx_type != req.tx_type ||
		    (cfg.rx_filter != HWTSTAMP_FILTER_ALL &&
		     cfg.rx_filter != HWTSTAMP_FILTER_PTP_V2_EVENT)) {
			return -1;
		}
	}

	return 0;
}

/* public methods */

int sk_interface_fd(void)
{
	int fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		pr_err("socket failed: %m");
		return -1;
	}
	return fd;
}

int sk_interface_index(int fd, const char *name)
{
	struct ifreq ifreq;
	int err;

	memset(&ifreq, 0, sizeof(ifreq));
	strncpy(ifreq.ifr_name, name, sizeof(ifreq.ifr_name) - 1);
	err = ioctl(fd, SIOCGIFINDEX, &ifreq);
	if (err < 0) {
		pr_err("ioctl SIOCGIFINDEX failed: %m");
		return err;
	}
	return ifreq.ifr_ifindex;
}

int sk_general_init(int fd)
{
	int on = sk_check_fupsync ? 1 : 0;
	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) < 0) {
		pr_err("ioctl SO_TIMESTAMPNS failed: %m");
		return -1;
	}
	return 0;
}

int sk_get_ts_info(const char *name, struct sk_ts_info *sk_info)
{
#ifdef ETHTOOL_GET_TS_INFO
	struct ethtool_ts_info info;
	struct ifreq ifr;
	int fd, err;

	memset(&ifr, 0, sizeof(ifr));
	memset(&info, 0, sizeof(info));
	info.cmd = ETHTOOL_GET_TS_INFO;
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	ifr.ifr_data = (char *) &info;
	fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0) {
		pr_err("socket failed: %m");
		goto failed;
	}

	err = ioctl(fd, SIOCETHTOOL, &ifr);
	if (err < 0) {
		pr_err("ioctl SIOCETHTOOL failed: %m");
		close(fd);
		goto failed;
	}

	close(fd);

	/* copy the necessary data to sk_info */
	memset(sk_info, 0, sizeof(struct sk_ts_info));
	sk_info->valid = 1;
	sk_info->phc_index = info.phc_index;
	sk_info->so_timestamping = info.so_timestamping;
	sk_info->tx_types = info.tx_types;
	sk_info->rx_filters = info.rx_filters;

	return 0;
failed:
#endif
	/* clear data and ensure it is not marked valid */
	memset(sk_info, 0, sizeof(struct sk_ts_info));
	return -1;
}

int sk_interface_macaddr(const char *name, struct address *mac)
{
	struct ifreq ifreq;
	int err, fd;

	memset(&ifreq, 0, sizeof(ifreq));
	strncpy(ifreq.ifr_name, name, sizeof(ifreq.ifr_name) - 1);

	fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		pr_err("socket failed: %m");
		return -1;
	}

	err = ioctl(fd, SIOCGIFHWADDR, &ifreq);
	if (err < 0) {
		pr_err("ioctl SIOCGIFHWADDR failed: %m");
		close(fd);
		return -1;
	}

	mac->sll.sll_family = AF_PACKET;
	mac->sll.sll_halen = MAC_LEN;
	memcpy(mac->sll.sll_addr, &ifreq.ifr_hwaddr.sa_data, MAC_LEN);
	mac->len = sizeof(mac->sll);
	close(fd);
	return 0;
}

int sk_interface_addr(const char *name, int family, struct address *addr)
{
	struct ifaddrs *ifaddr, *i;
	int result = -1;

	if (getifaddrs(&ifaddr) == -1) {
		pr_err("getifaddrs failed: %m");
		return -1;
	}
	for (i = ifaddr; i; i = i->ifa_next) {
		if (i->ifa_addr && family == i->ifa_addr->sa_family &&
			strcmp(name, i->ifa_name) == 0)
		{
			switch (family) {
			case AF_INET:
				addr->len = sizeof(addr->sin);
				memcpy(&addr->sin, i->ifa_addr, addr->len);
				break;
			case AF_INET6:
				addr->len = sizeof(addr->sin6);
				memcpy(&addr->sin6, i->ifa_addr, addr->len);
				break;
			default:
				continue;
			}
			result = 0;
			break;
		}
	}
	freeifaddrs(ifaddr);
	return result;
}

static short sk_events = POLLPRI;
static short sk_revents = POLLPRI;

#ifdef SJA1105_TC
static int sk_receive_meta(int fd, struct address *addr, struct meta_data *meta)
{
	char data[sizeof(struct eth_hdr) + 8];
	char control[256];
	struct msghdr msg;
	struct iovec iov = { data, sizeof(data) };
	struct pollfd fd_meta = { fd, POLLIN|POLLPRI, 0 };
	int cnt, res;

	memset(control, 0, sizeof(control));
	memset(&msg, 0, sizeof(msg));

	if (addr) {
		msg.msg_name = &addr->ss;
		msg.msg_namelen = sizeof(addr->ss);
	}
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;

	res = poll(&fd_meta, 1, sk_meta_timeout);
	if (res <= 0) {
		printf("failed to poll fd_meta, or time out\n");
		return -1;
	}

	cnt = recvmsg(fd, &msg, 0);
	if (cnt < 1) {
		printf("failed to receive meta frame!\n");
		return -1;
	}

	memcpy(meta, &data[sizeof(struct eth_hdr)], sizeof(struct meta_data));

/*
	printf("receive meta frame:");
	int i;
	for (i = 0; i < 8; i++)
		printf("%x ", data[sizeof(struct eth_hdr) + i]);
	printf("\n");
*/
	return 0;
}

void ptp_insert_correction(struct ptp_message *m)
{
	struct tc *clock = &tc;

	switch (m->header.tsmt & 0x0f) {
	case FOLLOW_UP:
		if (!clock->master_setup)
			return;

		if (!clock->interface->sync)
			return;

		if (clock->interface->sync->header.sequenceId ==
					ntohs(m->header.sequenceId)) {
			m->header.correction = sync_tx_ts.tx_ts;
			m->header.correction = host2net64(m->header.correction);
		}
		break;
	}
}
#endif

int sk_receive(int fd, void *buf, int buflen,
	       struct address *addr, struct hw_timestamp *hwts, int flags)
{
#ifdef SJA1105_TC
	char control[256];
	int cnt = 0, res = 0;
	struct iovec iov = { buf, buflen };
	struct msghdr msg;

	struct host_if *interface = &tc_host_if;
	struct meta_data meta;
	struct ptp_message *ptp_msg;
	int cnt_send;
	struct sja1105_mgmt_entry sja1105_mgmt;
	struct timespec ts, tx_ts;
	uint64_t rx_ts;

	if (sja1105_ptp_clk_get(&spi_setup, &ts)) {
		printf("failed to get sja1105 clock for rx timestamp!\n");
		return -1;
	}

#else
	char control[256];
	int cnt = 0, res = 0, level, type;
	struct cmsghdr *cm;
	struct iovec iov = { buf, buflen };
	struct msghdr msg;
	struct timespec *sw, *ts = NULL;
#endif

	memset(control, 0, sizeof(control));
	memset(&msg, 0, sizeof(msg));
	if (addr) {
		msg.msg_name = &addr->ss;
		msg.msg_namelen = sizeof(addr->ss);
	}
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	if (flags == MSG_ERRQUEUE) {
		struct pollfd pfd = { fd, sk_events, 0 };
		res = poll(&pfd, 1, sk_tx_timeout);
		if (res < 1) {
			pr_err(res ? "poll for tx timestamp failed: %m" :
			             "timed out while polling for tx timestamp");
			pr_err("increasing tx_timestamp_timeout may correct "
			       "this issue, but it is likely caused by a driver bug");
			return res;
		} else if (!(pfd.revents & sk_revents)) {
			pr_err("poll for tx timestamp woke up on non ERR event");
			return -1;
		}
	}

	cnt = recvmsg(fd, &msg, flags);
	if (cnt < 1)
		pr_err("recvmsg%sfailed: %m",
		       flags == MSG_ERRQUEUE ? " tx timestamp " : " ");
#ifdef SJA1105_TC
	else {
		if (flags != MSG_ERRQUEUE) {
			if (sk_receive_meta(interface->fd_array.fd[FD_META], addr, &meta))
				return -1;

			sja1105_mgmt.destports = SJA1105_PORT & ~SJA1105_PORT_HOST &
						    ~(1 << meta.src_port);
			sja1105_mgmt.macaddr = PTP_E2E_ETH_MULTI_ADDR;
			sja1105_mgmt.ts_regid = 0;
			sja1105_mgmt.egr_ts = 1;

			if (sja1105_mgmt_route_set(&spi_setup, &sja1105_mgmt, 0))
				return -1;

			ptp_msg = buf + sizeof(struct eth_hdr);

			ptp_insert_correction(ptp_msg);

			cnt_send = send(fd, buf, sizeof(struct eth_hdr) +
				ntohs(ptp_msg->header.messageLength), 0);
			if (cnt_send < 1) {
				printf("failed to forward message!\n");
				return -1;
			}

			memset(&egress_ts_tmp, 0, sizeof(egress_ts_tmp));

			if (!sja1105_ptpegr_ts_poll(&spi_setup,
					sja1105_mgmt.destports & 0x1 ? 0 : 1,
					0, &tx_ts)) {
				egress_ts_tmp.tx_ts = tx_ts.tv_sec * NS_PER_SEC + tx_ts.tv_nsec;
				egress_ts_tmp.available = 1;
			} else
				printf("no updated tx timestamp!\n");
		}
	}
#endif

#ifndef SJA1105_TC
	for (cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm)) {
		level = cm->cmsg_level;
		type  = cm->cmsg_type;
		if (SOL_SOCKET == level && SO_TIMESTAMPING == type) {
			if (cm->cmsg_len < sizeof(*ts) * 3) {
				pr_warning("short SO_TIMESTAMPING message");
				return -1;
			}
			ts = (struct timespec *) CMSG_DATA(cm);
		}
		if (SOL_SOCKET == level && SO_TIMESTAMPNS == type) {
			if (cm->cmsg_len < sizeof(*sw)) {
				pr_warning("short SO_TIMESTAMPNS message");
				return -1;
			}
			sw = (struct timespec *) CMSG_DATA(cm);
			hwts->sw = *sw;
		}
	}
#endif

	if (addr)
		addr->len = msg.msg_namelen;

#ifdef SJA1105_TC
	rx_ts = (ts.tv_sec *NS_PER_SEC + ts.tv_nsec) / 8;
	rx_ts &= ~0xffffff;
	rx_ts |= meta.rx_ts_byte2 << 16 |
		 meta.rx_ts_byte1 << 8 |
		 meta.rx_ts_byte0;

	hwts->ts.tv_sec = (rx_ts * 8) / 1000000000;
	hwts->ts.tv_nsec = (rx_ts * 8) % 1000000000;
#else
	if (!ts) {
		memset(&hwts->ts, 0, sizeof(hwts->ts));
		return cnt;
	}

	switch (hwts->type) {
	case TS_SOFTWARE:
		hwts->ts = ts[0];
		break;
	case TS_HARDWARE:
	case TS_ONESTEP:
		hwts->ts = ts[2];
		break;
	case TS_LEGACY_HW:
		hwts->ts = ts[1];
		break;
	}
#endif
	return cnt;
}

int sk_set_priority(int fd, uint8_t dscp)
{
	int tos;
	socklen_t tos_len;

	tos_len = sizeof(tos);
	if (getsockopt(fd, SOL_IP, IP_TOS, &tos, &tos_len) < 0) {
		tos = 0;
	}

	/* clear old DSCP value */
	tos &= ~0xFC;

	/* set new DSCP value */
	tos |= dscp<<2;
	tos_len = sizeof(tos);
	if (setsockopt(fd, SOL_IP, IP_TOS, &tos, tos_len) < 0) {
		return -1;
	}

	return 0;
}

int sk_timestamping_init(int fd, const char *device, enum timestamp_type type,
			 enum transport_type transport)
{
	int err, filter1, filter2 = 0, flags, one_step;

	switch (type) {
	case TS_SOFTWARE:
		flags = SOF_TIMESTAMPING_TX_SOFTWARE |
			SOF_TIMESTAMPING_RX_SOFTWARE |
			SOF_TIMESTAMPING_SOFTWARE;
		break;
	case TS_HARDWARE:
	case TS_ONESTEP:
		flags = SOF_TIMESTAMPING_TX_HARDWARE |
			SOF_TIMESTAMPING_RX_HARDWARE |
			SOF_TIMESTAMPING_RAW_HARDWARE;
		break;
	case TS_LEGACY_HW:
		flags = SOF_TIMESTAMPING_TX_HARDWARE |
			SOF_TIMESTAMPING_RX_HARDWARE |
			SOF_TIMESTAMPING_SYS_HARDWARE;
		break;
	default:
		return -1;
	}

	if (type != TS_SOFTWARE) {
		filter1 = HWTSTAMP_FILTER_PTP_V2_EVENT;
		one_step = type == TS_ONESTEP ? 1 : 0;
		switch (transport) {
		case TRANS_UDP_IPV4:
		case TRANS_UDP_IPV6:
			filter2 = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
			break;
		case TRANS_IEEE_802_3:
			filter2 = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
			break;
		case TRANS_DEVICENET:
		case TRANS_CONTROLNET:
		case TRANS_PROFINET:
		case TRANS_UDS:
			return -1;
		}
		err = hwts_init(fd, device, filter1, one_step);
		if (err) {
			pr_info("driver rejected most general HWTSTAMP filter");
			err = hwts_init(fd, device, filter2, one_step);
			if (err) {
				pr_err("ioctl SIOCSHWTSTAMP failed: %m");
				return err;
			}
		}
	}

	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING,
		       &flags, sizeof(flags)) < 0) {
		pr_err("ioctl SO_TIMESTAMPING failed: %m");
		return -1;
	}

	flags = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_SELECT_ERR_QUEUE,
		       &flags, sizeof(flags)) < 0) {
		pr_warning("%s: SO_SELECT_ERR_QUEUE: %m", device);
		sk_events = 0;
		sk_revents = POLLERR;
	}

	/* Enable the sk_check_fupsync option, perhaps. */
	if (sk_general_init(fd)) {
		return -1;
	}

	return 0;
}
