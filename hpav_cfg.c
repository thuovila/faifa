/*
 * Lightweight Homeplug AV configuration utility
 *
 * Generates a NMK from a given passphrase
 *
 * Copyright (C) 2012, Florian Fainelli <florian@openwrt.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <linux/if_ether.h>

#include "homeplug_av.h"
#include "crypto.h"

struct context {
	int sock_fd;
	int if_index;
};

static int send_pkt(struct context *ctx, const uint8_t *to,
			const void *hdr, size_t hdrlen,
			const void *payload, size_t payload_len)
{
	struct sockaddr_ll ll;
	struct msghdr msg;
	struct iovec iov[3];
	size_t total_len;
	uint8_t padding[64];
	int ret;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	total_len = 0;

	iov[msg.msg_iovlen].iov_base = (void *)hdr;
	iov[msg.msg_iovlen].iov_len = hdrlen;
	total_len += hdrlen;
	msg.msg_iovlen = 1;

	if (payload_len) {
		iov[msg.msg_iovlen].iov_base = (void *)payload;
		iov[msg.msg_iovlen].iov_len = payload_len;
		total_len += payload_len;
		msg.msg_iovlen++;
	}

	if (total_len < 64) {
		memset(padding, 0, sizeof(padding));
		iov[msg.msg_iovlen].iov_base = (void *)padding;
		iov[msg.msg_iovlen].iov_len = 64 - total_len;
		msg.msg_iovlen++;
	}

	memset(&ll, 0, sizeof(ll));
	ll.sll_family = AF_PACKET;
	ll.sll_ifindex = ctx->if_index;
	ll.sll_protocol = htons(ETHERTYPE_HOMEPLUG_AV);

	memcpy(ll.sll_addr, to, ETH_ALEN);
	msg.msg_name = &ll;
	msg.msg_namelen = sizeof(ll);

	ret = sendmsg(ctx->sock_fd, &msg, 0);
	if (ret < 0) {
		if (errno != EAGAIN)
			perror("sendmsg");
		return ret;
	}

	return 0;
}

struct homeplug_av_vendor_hdr {
	uint8_t mmver;
	uint16_t mmtype;
	uint8_t oui[3];
} __attribute__((__packed__));

static int send_vendor_pkt(struct context *ctx, const uint8_t *to,
				uint16_t mmtype, const void *payload, size_t payload_len)
{
	struct homeplug_av_vendor_hdr hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.mmver = 0;
	hdr.mmtype = mmtype;
	hdr.oui[0] = 0x00;
	hdr.oui[1] = 0xB0;
	hdr.oui[2] = 0x52;

	return send_pkt(ctx, to, &hdr, sizeof(hdr), payload, payload_len);
}

static int init_socket(struct context *ctx, const char *iface)
{
	int ret;
	struct sockaddr_ll ll;

	ctx->sock_fd = socket(PF_PACKET, SOCK_DGRAM, htons(ETHERTYPE_HOMEPLUG_AV));
	if (ctx->sock_fd < 0) {
		perror("socket");
		return -1;
	}

	ctx->if_index = if_nametoindex(iface);
	memset(&ll, 0, sizeof(ll));
	ll.sll_family = AF_PACKET;
	ll.sll_ifindex = ctx->if_index;
	ll.sll_protocol = htons(ETHERTYPE_HOMEPLUG_AV);

	ret = bind(ctx->sock_fd, (struct sockaddr *)&ll, sizeof(ll));
	if (ret < 0) {
		perror("bind");
		goto out_close;
	}

	return 0;

out_close:
	close(ctx->sock_fd);
	return ret;
}

static int send_key(struct context *ctx, const char *pass, const char *mac)
{
	struct set_encryption_key_request key_req;
	uint8_t key[16];
	uint8_t to[ETH_ALEN];

	sscanf(mac, "%02x:%02x:%02x:%02x:%02x:%02x",
		&to[0], &to[1], &to[2], &to[3], &to[4], &to[5]);

	memset(&key_req, 0, sizeof(key_req));

	key_req.peks = 0x01;
	key_req.peks_payload = NO_KEY;

	gen_passphrase(pass, key, nmk_salt);
	memcpy(key_req.nmk, key, AES_KEY_SIZE);
	memcpy(key_req.rdra, mac, ETH_ALEN);

	return send_vendor_pkt(ctx, to, HPAV_MMTYPE_SET_KEY_REQ,
				&key_req, sizeof(key_req));
}

static int read_key_confirm(struct context *ctx)
{
	uint8_t frame[ETH_DATA_LEN];
	ssize_t len;
	socklen_t sk_len;
	struct sockaddr_ll ll;
	struct hpav_frame *hpav_frame;
	uint8_t status;

	sk_len = sizeof(ll);
	len = recvfrom(ctx->sock_fd, frame, sizeof(frame), 0,
			(struct sockaddr *)&ll, &sk_len);
	if (len < 0 || len < HPAV_MIN_FRAMSIZ) {
		if (errno != EAGAIN)
			perror("recvfrom");
		return len;
	}

	hpav_frame = (struct hpav_frame *)frame;
	if (le16toh(hpav_frame->header.mmtype) != HPAV_MMTYPE_SET_KEY_CNF)
		return 1;

	status = hpav_frame->payload.vendor.data[0];
	if (status) {
		fprintf(stderr, "device replies: %02x\n", status);
		return 1;
	}

	fprintf(stdout, "Success!!\n");

	return 0;
}

static int generate_passphrase(struct context *ctx, const char *pass)
{
	uint8_t key[16];
	int i;

	if (!pass) {
		fprintf(stderr, "missing passphrase\n");
		return 1;
	}

	gen_passphrase(pass, key, nmk_salt);
	for (i = 0; i < sizeof(key); i++)
		fprintf(stdout, "%02x", key[i]);

	return 0;
}

int main(int argc, char **argv)
{
	int opt;
	int ret;
	const char *mac_address = NULL;
	const char *passphrase = NULL;
	const char *iface = NULL;
	struct context ctx;
	unsigned int hash_only = 0;

	memset(&ctx, 0, sizeof(ctx));

	while ((opt = getopt(argc, argv, "p:a:i:kh")) > 0) {
		switch (opt) {
		case 'p':
			passphrase = optarg;
			break;
		case 'a':
			mac_address = optarg;
			break;
		case 'i':
			iface = optarg;
			break;
		case 'k':
			hash_only = 1;
			break;
		case 'h':
		default:
			printf("hpav_cfg: -n passphrase -a address [iface]\n");
			return 1;
		}
	}

	argc -= optind;
	argv += optind;

	if (hash_only)
		return generate_passphrase(&ctx, passphrase);

	iface = argv[0];

	fprintf(stdout, "Passphrase: %s\n", passphrase);
	fprintf(stdout, "MAC: %s\n", mac_address);
	fprintf(stdout, "Interface: %s\n", iface);

	ret = init_socket(&ctx, iface);
	if (ret) {
		fprintf(stdout, "failed to initialize raw socket\n");
		return ret;
	}

	ret=  send_key(&ctx, passphrase, mac_address);
	if (ret) {
		fprintf(stdout, "failed to send key\n");
		return ret;
	}

	return read_key_confirm(&ctx);
}
