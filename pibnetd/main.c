/*
 * Copyright (c) 2014 Minoru NAKAMURA <nminoru@nminoru.jp>
 *
 * This code is licenced under the GPL version 2 or BSD license.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <getopt.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <net/if.h>

#include "pibnetd.h"
#include "pibnetd_packet.h"


uint64_t pib_hca_guid_base;

static int verbose;
static uint32_t port_num = PIB_NETD_DEFAULT_PORT;

static struct pib_switch *init_switch(void);
static void finish_switch(struct pib_switch *sw);
static void construct_hca_guid_base(int sockfd);
static void do_work(struct pib_switch *sw);
static void receive_packet(struct pib_switch *sw);
static void process_raw_packet(struct pib_switch *sw, uint64_t port_guid, struct sockaddr *sockaddr, void *buffer, int size);
static void resend_ack(struct pib_switch *sw, int size, uint8_t port_num);
static void send_trap_ntc128(struct pib_switch *sw);
static uint8_t detect_in_port(struct pib_switch *sw, uint64_t port_guid);
static int process_mad_packet(struct pib_switch *sw, uint8_t in_port_num, struct pib_packet_lrh *lrh, struct pib_packet_bth *bth, void *buffer, int size);
static void relay_unicast_packet(struct pib_switch *sw, uint8_t in_port_num, uint16_t dlid, int size);
static void relay_unicast_packet(struct pib_switch *sw, uint8_t in_port_num, uint16_t dlid, int size);
static void relay_multicast_packet(struct pib_switch *sw, uint8_t in_port_num, uint16_t dlid, int size);

static int parse_packet_header(void *buffer, int size, struct pib_packet_lrh **lrh_p, struct pib_grh **grh_p, struct pib_packet_bth **bth_p);
static int pib_is_unicast_lid(uint16_t lid);
static int pib_is_permissive_lid(uint16_t lid);


int main(int argc, char** argv)
{
	struct option longopts[] = {
		{"port",     required_argument, NULL, 'p' },
		{"verbose",  no_argument,       NULL, 'v' },
	};

	int ch, option_index;

	while ((ch = getopt_long(argc, argv, "p:v", longopts, &option_index)) != -1) {
		switch (ch) {

		case 'p':
			port_num = atoi(optarg);
			assert((0 < port_num) && (port_num < 65536));
			break;

		case 'v': // verbose
			verbose = 1;
			break;

		default:
			break;
		}
	}

	struct pib_switch *sw;
	sw = init_switch();
	pib_report_info("pibnetd: " PIB_SWITCH_DESCRIPTION " v" PIB_DRIVER_VERSION);
	do_work(sw);

	return 0;
}


static struct pib_switch *init_switch(void)
{
	int i, j, ret;
	struct sockaddr_in sockaddr;
	struct pib_switch *sw;

	sw = calloc(1, sizeof(*sw));
	assert(sw);

	sw->buffer = malloc(PIB_PACKET_BUFFER);
	assert(sw->buffer);

	sw->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sw->sockfd < 0) {
		int eno = errno;
		pib_report_err("pibnetd: socket(ret=%d)", eno);
		exit(EXIT_FAILURE);
	}

	construct_hca_guid_base(sw->sockfd);

	memset(&sockaddr, 0, sizeof(sockaddr));
	sockaddr.sin_family      = AF_INET;
	sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	sockaddr.sin_port        = htons(port_num);

	ret = bind(sw->sockfd, (struct sockaddr*)&sockaddr, (socklen_t)sizeof(sockaddr));
	if (ret != 0) {
		int eno  = errno;
		pib_report_err("pibnetd: bind(ret=%d)", eno);
		exit(EXIT_FAILURE);
	}

	sw->sockaddr = calloc(1, sizeof(sockaddr));

	memcpy(sw->sockaddr, &sockaddr, sizeof(sockaddr));

	sw->port_cnt = PIB_MAX_PORTS;

	for (i=0 ; i<PIB_MAX_PORTS ; i++) {
		struct ibv_port_attr port_attr = {
			.state           = IBV_PORT_DOWN,
			.max_mtu         = IBV_MTU_4096,
			.active_mtu      = IBV_MTU_256,
			.gid_tbl_len     = PIB_GID_PER_PORT,
			.port_cap_flags  = PIB_PORT_CAP_FLAGS,
			.max_msg_sz      = PIB_MAX_PAYLOAD_LEN,
			.bad_pkey_cntr   = 0U,
			.qkey_viol_cntr  = 0U,
			.pkey_tbl_len    = PIB_PKEY_TABLE_LEN,
			.lid             = 0U,
			.sm_lid          = 0U,
			.lmc             = 0U,
			.max_vl_num      = 4U,
			.sm_sl           = 0U,
			.subnet_timeout  = 0U,
			.init_type_reply = 0U,
			.active_width    = PIB_WIDTH_12X,
			.active_speed    = PIB_SPEED_QDR,
			.phys_state      = PIB_PHYS_PORT_POLLING,
		};

		struct pib_port* port;
		port = &sw->ports[i];

		port->port_num	    = i;
		port->ibv_port_attr = port_attr;
		port->gid[0].global.subnet_prefix =
			/* default GID prefix */
			htobe64(0xFE80000000000000ULL);
		/* the same guid for all ports on a switch */
		port->gid[0].global.interface_id  =
			htobe64(pib_hca_guid_base | 0x0101ULL);

		port->link_width_enabled = PIB_LINK_WIDTH_SUPPORTED;
		port->link_speed_enabled = PIB_LINK_SPEED_SUPPORTED;

		for (j=0 ; j < PIB_PKEY_PER_BLOCK ; j++)
			port->pkey_table[j] = cpu_to_be16(PIB_DEFAULT_PKEY_FULL);
	}

	sw->ucast_fwd_table = calloc(1, PIB_MCAST_LID_BASE);
	assert(sw->ucast_fwd_table);

	sw->mcast_fwd_table = calloc(sizeof(struct pib_port_bits), PIB_MAX_LID - PIB_MCAST_LID_BASE);
	assert(sw->mcast_fwd_table);

	return sw;
}


static void finish_switch(struct pib_switch *sw)
{
	close(sw->sockfd);
}


static void construct_hca_guid_base(int sockfd)
{
	uint64_t hwaddr = 0xCafeBabe0000ULL;
	struct ifaddrs *ifaddr, *ifa;

	if (getifaddrs(&ifaddr) < 0) {
		int eno  = errno;
		pib_report_err("pibnetd: getifaddrs(ret=%d)", eno);
		exit(EXIT_FAILURE);
	}

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL)
			continue;

		int family = ifa->ifa_addr->sa_family;

		if (family == AF_INET || family == AF_INET6) {
			struct ifreq ifr;

			memset(&ifr, 0, sizeof(ifr));
			ifr.ifr_addr.sa_family = family;
			strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ-1);

			ioctl(sockfd, SIOCGIFHWADDR, &ifr);

			int i;
			for (i=6 ; i<sizeof(ifr.ifr_hwaddr.sa_data) ; i++)
				if (ifr.ifr_hwaddr.sa_data[i] != 0)
					goto skip;

			hwaddr = (uint8_t)ifr.ifr_hwaddr.sa_data[0];
			for (i=1 ; i<6 ; i++) {
				hwaddr <<= 8;
				hwaddr |= (uint8_t)ifr.ifr_hwaddr.sa_data[i];
			}

			if (hwaddr != 0)
				goto done;
		}
	skip:
		;
	}

done:	
	freeifaddrs(ifaddr);

	pib_hca_guid_base = hwaddr;
}


static void do_work(struct pib_switch *sw)
{
	for (;;) {
		int ret, max = 0;
		fd_set rfds;

		FD_ZERO(&rfds);
		FD_SET(sw->sockfd, &rfds);

		if (max < sw->sockfd + 1)
			max = sw->sockfd + 1;
    
		struct timeval tv;
		tv.tv_sec  = 10;
		tv.tv_usec = 0;

	retry:
		ret = select(max, &rfds, NULL, NULL, &tv);
		if (ret < 0) {
			int eno = errno;
			if (eno == EINTR)
				goto retry;
			pib_report_err("pibnetd: select(errno=%d)", eno);
			exit(EXIT_FAILURE);
		} else if (ret > 0) {
			if (FD_ISSET(sw->sockfd, &rfds))
				receive_packet(sw);
		}
	}
}


static void receive_packet(struct pib_switch *sw)
{
	struct iovec iovec;
	struct msghdr msghdr;
	struct sockaddr_in sockaddr;

	memset(&msghdr,   0, sizeof(msghdr));
	memset(&sockaddr, 0, sizeof(sockaddr));

	iovec.iov_base     = sw->buffer;
	iovec.iov_len      = PIB_PACKET_BUFFER;

	msghdr.msg_name    = &sockaddr;
	msghdr.msg_namelen = sizeof(sockaddr);
	msghdr.msg_iov     = &iovec;
	msghdr.msg_iovlen  = 1;

	ssize_t size, packet_size;

retry:
	size = recvmsg(sw->sockfd, &msghdr, 0);
	if (size < 0) {
		int eno  = errno;
		if (eno == EINTR)
			goto retry;
		pib_report_err("pibnetd: recvmsg(errno=%d)", eno);
		exit(EXIT_FAILURE);
		return;
	}

	packet_size = size;

	char ipaddr[64];
	if (inet_ntop(AF_INET, &sockaddr.sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
		int eno  = errno;
		pib_report_err("pibnetd: inet_ntop(errno=%d)", eno);
		exit(EXIT_FAILURE);							
	}

	void *buffer;
	union pib_packet_footer *footer;

	buffer = sw->buffer;

	if (size < sizeof(*footer)) {
		pib_report_debug("pibnetd: no packet footer(size=%u)", size);
		return;
	}

	footer = sw->buffer + size - sizeof(*footer);

	size -= sizeof(*footer);

	uint64_t port_guid = be64toh(footer->pib.port_guid);

	int header_size;
	struct pib_packet_lrh *lrh = NULL;
	struct pib_grh        *grh = NULL;
	struct pib_packet_bth *bth = NULL;

	header_size = parse_packet_header(buffer, size, &lrh, &grh, &bth);
	if (header_size < 0) {
		pib_report_debug("pibnetd: wrong drop packet(size=%u)", size);
		return;
	}

	buffer += header_size;

	if ((lrh->sl_rsv_lnh & 0x3) == 0) {
		process_raw_packet(sw, port_guid, (struct sockaddr *)&sockaddr, buffer, size - header_size);
		return;
	}

	uint8_t in_port_num;
	in_port_num = detect_in_port(sw, port_guid);

	assert(in_port_num != 0);

	if (packet_size > 0) {
		struct pib_port* port;
		port = &sw->ports[in_port_num];

		port->perf.rcv_packets++;
		port->perf.rcv_data += packet_size;
	}

	uint32_t dest_qp_num;
	dest_qp_num = be32_to_cpu(bth->destQP);
	if (dest_qp_num & ~PIB_QPN_MASK) {
		pib_report_debug("pibnetd: drop packet: dest_qp_num=0x%06x", dest_qp_num);
		return;
	}

	uint16_t dlid = be16_to_cpu(lrh->dlid);
	if (dest_qp_num == PIB_QP0) {
		if (process_mad_packet(sw, in_port_num, lrh, bth, buffer, size - header_size)) {
			relay_unicast_packet(sw, in_port_num, dlid, size);
		}
		return;
	}

	if (!pib_is_permissive_lid(dlid) &&
	    (dlid != sw->ports[0].ibv_port_attr.lid)) {
		/* Switch 宛のパケットではない */
		if ((dest_qp_num == PIB_MULTICAST_QPN) || !pib_is_unicast_lid(dlid))
			relay_multicast_packet(sw, in_port_num, dlid, size);
		else
			relay_unicast_packet(sw, in_port_num, dlid, size);
		return;
	}

	if (dest_qp_num == PIB_QP1) {
		process_mad_packet(sw, in_port_num, lrh, bth, buffer, size - header_size);
		return;
	}

	/* MAD 以外の easy switch 宛のパケット */
	pib_report_debug("pibnetd: drop packet: dlid=0x%04x, dest_qp_num=0x%06x",
			 dlid, dest_qp_num);
	return;
}


static void process_raw_packet(struct pib_switch *sw, uint64_t port_guid, struct sockaddr *sockaddr, void *buffer, int size)
{
	int socklen;
	uint8_t port_num;
	uint16_t udp_port;
	struct pib_packet_link *link;
	char address[64];

	memset(address, 0, sizeof(address));

	switch (sockaddr->sa_family) {
	case AF_INET: {
		struct sockaddr_in *sockaddr_in = (struct sockaddr_in*)sockaddr;
		inet_ntop(AF_INET, &sockaddr_in->sin_addr, address, sizeof(address));
		udp_port = sockaddr_in->sin_port;
		socklen = sizeof(struct sockaddr_in);
		break;
	}

	case AF_INET6: {
		struct sockaddr_in6 *sockaddr_in6 = (struct sockaddr_in6*)sockaddr;
		inet_ntop(AF_INET6, &sockaddr_in6->sin6_addr, address, sizeof(address));
		udp_port = sockaddr_in6->sin6_port;
		socklen = sizeof(struct sockaddr_in6);
		break;
	}

	default:
		pib_report_err("pibnetd: sockaddr");
		exit(EXIT_FAILURE);		
		break;
	} 

	if (size < sizeof(*link))
		return;

	link = buffer;

	switch (be32_to_cpu(link->cmd)) {

	case PIB_LINK_CMD_CONNECT:
		port_num = detect_in_port(sw, port_guid);

		if (port_num != 0)
			break;

		for (port_num = 1 ; port_num < sw->port_cnt ; port_num++)
			if (sw->ports[port_num].port_guid == 0)
				goto found_new_port;

		pib_report_err("pibnetd: There is no empty port in this switch.");
		exit(EXIT_FAILURE);
		break;

	found_new_port:
		sw->ports[port_num].port_guid = port_guid;
		sw->ports[port_num].sockaddr  = malloc(socklen);
		sw->ports[port_num].socklen   = socklen;
		sw->ports[port_num].ibv_port_attr.state      = IBV_PORT_INIT;
		sw->ports[port_num].ibv_port_attr.phys_state = PIB_PHYS_PORT_LINK_UP;

		memcpy(sw->ports[port_num].sockaddr, sockaddr, socklen);

		link->cmd = cpu_to_be32(PIB_LINK_CMD_CONNECT_ACK);

		resend_ack(sw, size, port_num);
		send_trap_ntc128(sw);

		pib_report_info("pibnetd: link up port[%u]: port_guid=0x%" PRIx64 ", sock-addr=%s:%u",
				port_num, port_guid, address, udp_port);
		break;

	case PIB_LINK_CMD_DISCONNECT:
		port_num = detect_in_port(sw, port_guid);

		if (port_num == 0)
			break;

		pib_report_info("pibnetd: link down port[%u]: port_guid=0x%" PRIx64 ", sock-addr=%s:%u",
				port_num, port_guid, address, udp_port);

		link->cmd = cpu_to_be32(PIB_LINK_CMD_DISCONNECT_ACK);

		resend_ack(sw, size, port_num);
		send_trap_ntc128(sw);

		sw->ports[port_num].port_guid = 0;
		free(sw->ports[port_num].sockaddr);
		sw->ports[port_num].sockaddr  = NULL;
		sw->ports[port_num].socklen   = 0;
		sw->ports[port_num].ibv_port_attr.state      = IBV_PORT_DOWN;
		sw->ports[port_num].ibv_port_attr.phys_state = PIB_PHYS_PORT_POLLING;
		break;

	case PIB_LINK_SHUTDOWN:
		break;

	default:
		break;
	}
}


static void resend_ack(struct pib_switch *sw, int size, uint8_t port_num)
{
	struct iovec iovec;
	struct msghdr msghdr;

	memset(&msghdr, 0, sizeof(msghdr));
	memset(&iovec,  0, sizeof(iovec));

	iovec.iov_base     = sw->buffer;
	iovec.iov_len      = sizeof(struct pib_packet_lrh) + size + sizeof(union pib_packet_footer);
				
	msghdr.msg_name    = sw->ports[port_num].sockaddr;
	msghdr.msg_namelen = sw->ports[port_num].socklen;
	msghdr.msg_iov     = &iovec;
	msghdr.msg_iovlen  = 1;

	int ret;
retry:
	ret = sendmsg(sw->sockfd, &msghdr, 0);
	if (ret < 0) {
		int eno  = errno;
		if (eno == EINTR)
			goto retry;
		pib_report_err("pibnetd: sendmsg(errno=%d)", eno);
		exit(EXIT_FAILURE);
	}
}


static uint8_t detect_in_port(struct pib_switch *sw, uint64_t port_guid)
{
	uint8_t port_num;

	for (port_num = 1 ; port_num < sw->port_cnt ; port_num++)
		if (port_guid == sw->ports[port_num].port_guid)
			return port_num;

	return 0;
}


/**
 *  @retval  0  処理完了
 *  @retval -1  ユニキャスト転送
 */
static int process_mad_packet(struct pib_switch *sw, uint8_t in_port_num, struct pib_packet_lrh *lrh, struct pib_packet_bth *bth, void *buffer, int size)
{
	int ret;
	int self_consumed = 0;
	uint16_t dlid;
	uint8_t out_port_num = 0;
	struct iovec iovec;
	struct msghdr msghdr;
	struct pib_packet_deth *deth;
	struct pib_smp *smp;
	struct pib_mad *mad;

	ret  = size;

	deth = (struct pib_packet_deth*)buffer;
	if (ret < sizeof(*deth)) {
		pib_report_err("pibnetd: short of deth size: ret=%d, sizeof(deth)=%zu",
			       ret, sizeof(*deth));
		goto silently_drop;
	}

	buffer += sizeof(*deth);
	ret    -= sizeof(*deth);

	smp = (struct pib_smp*)buffer;
	mad = (struct pib_mad*)buffer;

	if (ret < sizeof(*mad)) {
		pib_report_err("pibnetd: short of mad size: ret=%d, sizeof(pib_mad)=%zu",
			ret, sizeof(*mad));
		goto silently_drop;
	}

	dlid = be16_to_cpu(lrh->dlid);

	switch (mad->mad_hdr.mgmt_class) {

	case PIB_MGMT_CLASS_SUBN_DIRECTED_ROUTE:
		if ((smp->dr_slid != cpu_to_be16(PIB_LID_PERMISSIVE)) ||
		    (smp->dr_dlid != cpu_to_be16(PIB_LID_PERMISSIVE))) {
			/* DR SLID と DR DLID が指定されてない場合には未対応 */
			pib_report_err("pibnetd: SUBN_DIRECTED_ROUTE dr_slid=0x%04x, dr_dlid=0x%04x",
				be16_to_cpu(smp->dr_slid), be16_to_cpu(smp->dr_dlid));
			exit(EXIT_FAILURE);
		}
		break;

	case PIB_MGMT_CLASS_SUBN_LID_ROUTED:
		if (dlid != sw->ports[0].ibv_port_attr.lid)
			/* ユニキャスト転送 */
			return -1;

		ret = pib_process_smp(smp, sw, in_port_num);

		if (ret & PIB_SMP_RESULT_CONSUMED)
			return 0;

		lrh->dlid = lrh->slid;
		lrh->slid = cpu_to_be16(dlid);
		if (ret & PIB_SMP_RESULT_FAILURE) {
			pib_report_err("pibnetd: process_smp: failure");
			goto silently_drop;
		}
		out_port_num = in_port_num;
		goto send_packet;

	case PIB_MGMT_CLASS_PERF_MGMT: {
#if 0
		struct pib_node node = {
			.port_count = sw->port_cnt,
			.port_start = 0,
			.ports      = sw->ports,
		};

		ret = pib_process_pma_mad(&node, in_port_num, mad, mad);
		lrh->dlid = lrh->slid;
		lrh->slid = dlid;
		if (ret & PIB_MAD_RESULT_FAILURE) {
			pib_report_err("pibnetd: process_smp: failure");
			goto silently_drop;
		}
		out_port_num = in_port_num;
#endif
		goto send_packet;
	}

	default:
		pib_report_err("pibnetd: mgmt_class = %u",
			mad->mad_hdr.mgmt_class);
		exit(EXIT_FAILURE);
		break;
	}

	self_consumed = 0;

	if ((smp->status & PIB_SMP_DIRECTION) == 0) {
		/* Outgoing SMP */
		if (smp->hop_ptr == smp->hop_cnt) {
			if (smp->dr_dlid == be16_to_cpu(PIB_LID_PERMISSIVE)) {
				smp->hop_ptr--;
				ret = pib_process_smp(smp, sw, in_port_num);
				out_port_num = in_port_num;
				self_consumed = 1;
			} else {
				pib_report_err("pibnetd: packet.smp.dr_dlid = 0x%04x",
					       be16_to_cpu(smp->dr_dlid));
				exit(EXIT_FAILURE);
			}
		} else if (smp->hop_ptr == smp->hop_cnt + 1) {
			smp->hop_ptr--;
			ret = pib_process_smp(smp, sw, in_port_num);
			out_port_num = in_port_num;
			self_consumed = 1;
		} else {
			ret = PIB_SMP_RESULT_SUCCESS;
			out_port_num = smp->initial_path[smp->hop_ptr + 1];
			smp->hop_ptr++;
		}
	} else {
		/* Returning SMP */
		ret = PIB_SMP_RESULT_SUCCESS;
		smp->hop_ptr--;
		out_port_num = smp->initial_path[smp->hop_ptr];
		smp->return_path[smp->hop_ptr] = out_port_num;
	}

	if (self_consumed) {
		lrh->dlid = lrh->slid;
		if (smp->dr_slid == be16_to_cpu(PIB_LID_PERMISSIVE))
			lrh->slid = be16_to_cpu(PIB_LID_PERMISSIVE);
	}

	if (ret & PIB_SMP_RESULT_FAILURE) {
		pib_report_err("pibnetd: process_smp: failure");
		goto silently_drop;
	}

send_packet:
	memset(&msghdr,   0, sizeof(msghdr));

	iovec.iov_base	   = sw->buffer;
	iovec.iov_len	   = pib_packet_lrh_get_pktlen(lrh) * 4 + sizeof(union pib_packet_footer);

	msghdr.msg_name    = sw->ports[out_port_num].sockaddr;
	msghdr.msg_namelen = sw->ports[out_port_num].socklen;
	msghdr.msg_iov     = &iovec;
	msghdr.msg_iovlen  = 1;

retry:
	ret = sendmsg(sw->sockfd, &msghdr, 0);
	if (ret < 0) {
		int eno = errno;
		if (eno == EINTR)
			goto retry;
		pib_report_err("pibnetd: sendmsg(ret=%d)", eno);
		exit(EXIT_FAILURE);
	}

	if (ret > 0) {
		sw->ports[out_port_num].perf.xmit_packets++;
		sw->ports[out_port_num].perf.xmit_data += ret;
	}

	return 0;

silently_drop:
	pib_report_debug("pibnetd: silently_drop");
	return 0;
}


static void relay_unicast_packet(struct pib_switch *sw, uint8_t in_port_num, uint16_t dlid, int size)
{
	int ret;
	uint8_t out_port_num;
	struct iovec iovec;
	struct msghdr msghdr;

	out_port_num = sw->ucast_fwd_table[dlid];
	if (out_port_num == 0)
		return;

	memset(&msghdr,   0, sizeof(msghdr));

	iovec.iov_base	   = sw->buffer;
	iovec.iov_len	   = size + sizeof(union pib_packet_footer);

	msghdr.msg_name    = sw->ports[out_port_num].sockaddr;
	msghdr.msg_namelen = sw->ports[out_port_num].socklen;
	msghdr.msg_iov     = &iovec;
	msghdr.msg_iovlen  = 1;

retry:
	ret = sendmsg(sw->sockfd, &msghdr, 0);
	if (ret < 0) {
		int eno = errno;
		if (eno == EINTR)
			goto retry;
		pib_report_err("pibnetd: sendmsg(ret=%d)", eno);
		exit(EXIT_FAILURE);
	}

	if (ret > 0) {
		sw->ports[out_port_num].perf.xmit_packets++;
		sw->ports[out_port_num].perf.xmit_data += ret;
	}
}


static void relay_multicast_packet(struct pib_switch *sw, uint8_t in_port_num, uint16_t dlid, int size)
{
	uint8_t out_port_num;

	for (out_port_num = 1 ; out_port_num < sw->port_cnt ; out_port_num++) {
		uint16_t pm_block;

		/* マルチキャストグループに属していても入力ポートへは送信しない */
		if (in_port_num == out_port_num)
			continue;

		/* マルチキャストグループの出力ポートではない */
		pm_block = sw->mcast_fwd_table[dlid - PIB_MCAST_LID_BASE].pm_blocks[out_port_num / 16];
		if ((pm_block & (1U << (out_port_num % 16))) == 0)
			continue;

		int ret;
		struct iovec iovec;
		struct msghdr msghdr;

		memset(&msghdr,   0, sizeof(msghdr));

		iovec.iov_base	   = sw->buffer;
		iovec.iov_len	   = size + sizeof(union pib_packet_footer);

		msghdr.msg_name    = sw->ports[out_port_num].sockaddr;
		msghdr.msg_namelen = sw->ports[out_port_num].socklen;
		msghdr.msg_iov     = &iovec;
		msghdr.msg_iovlen  = 1;

	retry:
		ret = sendmsg(sw->sockfd, &msghdr, 0);
		if (ret < 0) {
			int eno = errno;
			if (eno == EINTR)
				goto retry;
			pib_report_err("pibnetd: sendmsg(ret=%d)", eno);
			exit(EXIT_FAILURE);
		}

		if (ret > 0) {
			sw->ports[out_port_num].perf.xmit_packets++;
			sw->ports[out_port_num].perf.xmit_data += ret;
		}
	}
}


static int parse_packet_header(void *buffer, int size, struct pib_packet_lrh **lrh_p, struct pib_grh **grh_p, struct pib_packet_bth **bth_p)
{
	int ret = 0;
	struct pib_packet_lrh *lrh;
	struct pib_grh        *grh = NULL;
	struct pib_packet_bth *bth;
	uint8_t lnh;

	/* Analyze Local Route Hedaer */
	lrh = (struct pib_packet_lrh *)buffer;

	if (size < sizeof(*lrh))
		return -1;

	/* check packet length */
	if (pib_packet_lrh_get_pktlen(lrh) * 4 != size)
		return -1;

	buffer += sizeof(*lrh);
	size   -= sizeof(*lrh);
	ret    += sizeof(*lrh); 

	/* check Link Version */
	if ((lrh->vl_lver & 0xF) != 0)
		return -2;

	/* check Transport &  Next Header */
	lnh = (lrh->sl_rsv_lnh & 0x3);

	if (lnh == 0x2)
		/* IBA local */
		goto skip_grh;
	else if (lnh == 0) {
		/* Raw packt */
		*lrh_p = lrh;
		return ret;
	} else if (lnh != 0x3)
		return -3;

	/* IBA global */
	grh = (struct pib_grh *)buffer;

	if (size < sizeof(*grh))
		return -1;

	buffer += sizeof(*grh);
	size   -= sizeof(*grh);
	ret    += sizeof(*grh); 

skip_grh:
	/* Base Transport Header */
	bth = (struct pib_packet_bth *)buffer;
	    
	if (size < sizeof(*bth))
		return -1;

	ret    += sizeof(*bth);

	*lrh_p = lrh;
	*grh_p = grh;
	*bth_p = bth;

	return ret;
}


static void send_trap_ntc128(struct pib_switch *sw)
{
	uint16_t slid, dlid;

	slid = sw->ports[0].ibv_port_attr.lid;
	dlid = sw->ports[0].ibv_port_attr.sm_lid;

	if ((slid == 0) || (dlid == 0))
		return;

	uint8_t out_port_num = sw->ucast_fwd_table[dlid];

	printf("slid=%04x dlid=%04x outgoing-port=%u\n", slid, dlid, out_port_num);

	if (out_port_num == 0)
		return;

	struct packet {
		struct pib_packet_lrh  lrh;
		struct pib_packet_bth  bth;
		struct pib_packet_deth deth;
		struct pib_smp         smp;
	} *packet = sw->buffer;

	memset(packet, 0, sizeof(*packet));

	packet->lrh.dlid       = cpu_to_be16(dlid);
	packet->lrh.sl_rsv_lnh = 0x2;
	packet->lrh.slid       = cpu_to_be16(slid);
	packet->bth.OpCode     = 0x64; /* IB_OPCODE_UD_SEND_ONLY */
	packet->bth.pkey       = 0;
	packet->bth.destQP     = cpu_to_be32(PIB_QP0);
	packet->deth.qkey      = 0;
	packet->deth.srcQP     = cpu_to_be32(PIB_QP0);

	packet->smp.base_version = PIB_MGMT_BASE_VERSION;
	packet->smp.mgmt_class = PIB_MGMT_CLASS_SUBN_LID_ROUTED;
	packet->smp.class_version = PIB_MGMT_CLASS_VERSION;
	packet->smp.method     = PIB_MGMT_METHOD_TRAP;
	packet->smp.status     = 0;
	packet->smp.tid        = cpu_to_be64(1);
	packet->smp.attr_id    = cpu_to_be16(PIB_SMP_ATTR_NOTICE);
	packet->smp.attr_mod   = 0;

	pib_packet_lrh_set_pktlen(&packet->lrh, sizeof(*packet) / 4);

	struct pib_trap *trap;

	trap = (struct pib_trap *)packet->smp.data;

	trap->generice_type_prodtype = cpu_to_be32((1U << 31) | (1U /* Urgent */ << 24) | 2 /* swich */);
	trap->trapnum = cpu_to_be16(128);
	trap->issuerlid = cpu_to_be16(slid);
	trap->details.ntc_128.lidaddr = cpu_to_be16(slid);

	struct iovec iovec;
	struct msghdr msghdr;

	memset(&msghdr, 0, sizeof(msghdr));
	memset(&iovec,  0, sizeof(iovec));

	iovec.iov_base     = sw->buffer;
	iovec.iov_len      = sizeof(*packet) + sizeof(union pib_packet_footer);
				
	msghdr.msg_name    = sw->ports[out_port_num].sockaddr;
	msghdr.msg_namelen = sw->ports[out_port_num].socklen;
	msghdr.msg_iov     = &iovec;
	msghdr.msg_iovlen  = 1;

	int ret;
retry:
	ret = sendmsg(sw->sockfd, &msghdr, 0);
	if (ret < 0) {
		int eno  = errno;
		if (eno == EINTR)
			goto retry;
		pib_report_err("pibnetd: sendmsg(errno=%d)", eno);
		exit(EXIT_FAILURE);
	}
}


static int pib_is_unicast_lid(uint16_t lid)
{
	return (lid < PIB_MCAST_LID_BASE) || (lid == PIB_LID_PERMISSIVE);
}


static int pib_is_permissive_lid(uint16_t lid)
{
	return (lid == 0) || (lid == PIB_LID_PERMISSIVE);
}
