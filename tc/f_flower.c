/*
 * f_flower.c		Flower Classifier
 *
 *		This program is free software; you can distribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:     Jiri Pirko <jiri@resnulli.us>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <net/if.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tc_act/tc_vlan.h>

#include "utils.h"
#include "tc_util.h"
#include "rt_names.h"

static void explain(void)
{
	fprintf(stderr,
		"Usage: ... flower [ MATCH-LIST ]\n"
		"                  [ skip_sw | skip_hw ]\n"
		"                  [ action ACTION-SPEC ] [ classid CLASSID ]\n"
		"\n"
		"Where: MATCH-LIST := [ MATCH-LIST ] MATCH\n"
		"       MATCH      := { indev DEV-NAME |\n"
		"                       vlan_id VID |\n"
		"                       vlan_prio PRIORITY |\n"
		"                       vlan_ethtype [ ipv4 | ipv6 | ETH-TYPE ] |\n"
		"                       dst_mac MAC-ADDR |\n"
		"                       src_mac MAC-ADDR |\n"
		"                       ip_proto [tcp | udp | IP-PROTO ] |\n"
		"                       dst_ip [ IPV4-ADDR | IPV6-ADDR ] |\n"
		"                       src_ip [ IPV4-ADDR | IPV6-ADDR ] |\n"
		"                       dst_port PORT-NUMBER |\n"
		"                       src_port PORT-NUMBER }\n"
		"       FILTERID := X:Y:Z\n"
		"       ACTION-SPEC := ... look at individual actions\n"
		"\n"
		"NOTE: CLASSID, IP-PROTO are parsed as hexadecimal input.\n"
		"NOTE: There can be only used one mask per one prio. If user needs\n"
		"      to specify different mask, he has to use different prio.\n");
}

static int flower_parse_eth_addr(char *str, int addr_type, int mask_type,
				 struct nlmsghdr *n)
{
	int ret;
	char addr[ETH_ALEN];

	ret = ll_addr_a2n(addr, sizeof(addr), str);
	if (ret < 0)
		return -1;
	addattr_l(n, MAX_MSG, addr_type, addr, sizeof(addr));
	memset(addr, 0xff, ETH_ALEN);
	addattr_l(n, MAX_MSG, mask_type, addr, sizeof(addr));
	return 0;
}

static int flower_parse_vlan_eth_type(char *str, __be16 eth_type, int type,
				      __be16 *p_vlan_eth_type,
				      struct nlmsghdr *n)
{
	__be16 vlan_eth_type;

	if (eth_type != htons(ETH_P_8021Q)) {
		fprintf(stderr,
			"Can't set \"vlan_ethtype\" if ethertype isn't 802.1Q\n");
		return -1;
	}

	if (ll_proto_a2n(&vlan_eth_type, str))
		invarg("invalid vlan_ethtype", str);
	addattr16(n, MAX_MSG, type, vlan_eth_type);
	*p_vlan_eth_type = vlan_eth_type;
	return 0;
}

static int flower_parse_ip_proto(char *str, __be16 eth_type, int type,
				 __u8 *p_ip_proto, struct nlmsghdr *n)
{
	int ret;
	__u8 ip_proto;

	if (eth_type != htons(ETH_P_IP) && eth_type != htons(ETH_P_IPV6)) {
		fprintf(stderr, "Illegal \"eth_type\" for ip proto\n");
		return -1;
	}
	if (matches(str, "tcp") == 0) {
		ip_proto = IPPROTO_TCP;
	} else if (matches(str, "udp") == 0) {
		ip_proto = IPPROTO_UDP;
	} else if (matches(str, "sctp") == 0) {
		ip_proto = IPPROTO_SCTP;
	} else {
		ret = get_u8(&ip_proto, str, 16);
		if (ret)
			return -1;
	}
	addattr8(n, MAX_MSG, type, ip_proto);
	*p_ip_proto = ip_proto;
	return 0;
}

static int flower_parse_ip_addr(char *str, __be16 eth_type,
				int addr4_type, int mask4_type,
				int addr6_type, int mask6_type,
				struct nlmsghdr *n)
{
	int ret;
	inet_prefix addr;
	int family;
	int bits;
	int i;

	if (eth_type == htons(ETH_P_IP)) {
		family = AF_INET;
	} else if (eth_type == htons(ETH_P_IPV6)) {
		family = AF_INET6;
	} else {
		fprintf(stderr, "Illegal \"eth_type\" for ip address\n");
		return -1;
	}

	ret = get_prefix(&addr, str, family);
	if (ret)
		return -1;

	if (addr.family != family)
		return -1;

	addattr_l(n, MAX_MSG, addr.family == AF_INET ? addr4_type : addr6_type,
		  addr.data, addr.bytelen);

	memset(addr.data, 0xff, addr.bytelen);
	bits = addr.bitlen;
	for (i = 0; i < addr.bytelen / 4; i++) {
		if (!bits) {
			addr.data[i] = 0;
		} else if (bits / 32 >= 1) {
			bits -= 32;
		} else {
			addr.data[i] <<= 32 - bits;
			addr.data[i] = htonl(addr.data[i]);
			bits = 0;
		}
	}

	addattr_l(n, MAX_MSG, addr.family == AF_INET ? mask4_type : mask6_type,
		  addr.data, addr.bytelen);

	return 0;
}

static int flower_port_attr_type(__u8 ip_port, bool is_src)
{
	if (ip_port == IPPROTO_TCP) {
		return is_src ? TCA_FLOWER_KEY_TCP_SRC :
			TCA_FLOWER_KEY_TCP_DST;
	} else if (ip_port == IPPROTO_UDP) {
		return is_src ? TCA_FLOWER_KEY_UDP_SRC :
			TCA_FLOWER_KEY_UDP_DST;
	} else if (ip_port == IPPROTO_SCTP) {
		return is_src ? TCA_FLOWER_KEY_SCTP_SRC :
			TCA_FLOWER_KEY_SCTP_DST;
	} else {
		fprintf(stderr, "Illegal \"ip_proto\" for port\n");
		return -1;
	}
}

static int flower_parse_port(char *str, __u8 ip_port, bool is_src,
			     struct nlmsghdr *n)
{
	int ret;
	int type;
	__be16 port;

	type = flower_port_attr_type(ip_port, is_src);
	if (type < 0)
		return -1;

	ret = get_be16(&port, str, 10);
	if (ret)
		return -1;

	addattr16(n, MAX_MSG, type, port);

	return 0;
}

static int flower_parse_opt(struct filter_util *qu, char *handle,
			    int argc, char **argv, struct nlmsghdr *n)
{
	int ret;
	struct tcmsg *t = NLMSG_DATA(n);
	struct rtattr *tail;
	__be16 eth_type = TC_H_MIN(t->tcm_info);
	__be16 vlan_ethtype = 0;
	__u8 ip_proto = 0xff;
	__u32 flags = 0;

	if (handle) {
		ret = get_u32(&t->tcm_handle, handle, 0);
		if (ret) {
			fprintf(stderr, "Illegal \"handle\"\n");
			return -1;
		}
	}

	tail = (struct rtattr *) (((void *) n) + NLMSG_ALIGN(n->nlmsg_len));
	addattr_l(n, MAX_MSG, TCA_OPTIONS, NULL, 0);

	if (argc == 0) {
		/*at minimal we will match all ethertype packets */
		goto parse_done;
	}

	while (argc > 0) {
		if (matches(*argv, "classid") == 0 ||
		    matches(*argv, "flowid") == 0) {
			unsigned int handle;

			NEXT_ARG();
			ret = get_tc_classid(&handle, *argv);
			if (ret) {
				fprintf(stderr, "Illegal \"classid\"\n");
				return -1;
			}
			addattr_l(n, MAX_MSG, TCA_FLOWER_CLASSID, &handle, 4);
		} else if (matches(*argv, "skip_hw") == 0) {
			flags |= TCA_CLS_FLAGS_SKIP_HW;
		} else if (matches(*argv, "skip_sw") == 0) {
			flags |= TCA_CLS_FLAGS_SKIP_SW;
		} else if (matches(*argv, "indev") == 0) {
			char ifname[IFNAMSIZ] = {};

			NEXT_ARG();
			strncpy(ifname, *argv, sizeof(ifname) - 1);
			addattrstrz(n, MAX_MSG, TCA_FLOWER_INDEV, ifname);
		} else if (matches(*argv, "vlan_id") == 0) {
			__u16 vid;

			NEXT_ARG();
			if (eth_type != htons(ETH_P_8021Q)) {
				fprintf(stderr,
					"Can't set \"vlan_id\" if ethertype isn't 802.1Q\n");
				return -1;
			}
			ret = get_u16(&vid, *argv, 10);
			if (ret < 0 || vid & ~0xfff) {
				fprintf(stderr, "Illegal \"vlan_id\"\n");
				return -1;
			}
			addattr16(n, MAX_MSG, TCA_FLOWER_KEY_VLAN_ID, vid);
		} else if (matches(*argv, "vlan_prio") == 0) {
			__u8 vlan_prio;

			NEXT_ARG();
			if (eth_type != htons(ETH_P_8021Q)) {
				fprintf(stderr,
					"Can't set \"vlan_prio\" if ethertype isn't 802.1Q\n");
				return -1;
			}
			ret = get_u8(&vlan_prio, *argv, 10);
			if (ret < 0 || vlan_prio & ~0x7) {
				fprintf(stderr, "Illegal \"vlan_prio\"\n");
				return -1;
			}
			addattr8(n, MAX_MSG,
				 TCA_FLOWER_KEY_VLAN_PRIO, vlan_prio);
		} else if (matches(*argv, "vlan_ethtype") == 0) {
			NEXT_ARG();
			ret = flower_parse_vlan_eth_type(*argv, eth_type,
						 TCA_FLOWER_KEY_VLAN_ETH_TYPE,
						 &vlan_ethtype, n);
			if (ret < 0)
				return -1;
		} else if (matches(*argv, "dst_mac") == 0) {
			NEXT_ARG();
			ret = flower_parse_eth_addr(*argv,
						    TCA_FLOWER_KEY_ETH_DST,
						    TCA_FLOWER_KEY_ETH_DST_MASK,
						    n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"dst_mac\"\n");
				return -1;
			}
		} else if (matches(*argv, "src_mac") == 0) {
			NEXT_ARG();
			ret = flower_parse_eth_addr(*argv,
						    TCA_FLOWER_KEY_ETH_SRC,
						    TCA_FLOWER_KEY_ETH_SRC_MASK,
						    n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"src_mac\"\n");
				return -1;
			}
		} else if (matches(*argv, "ip_proto") == 0) {
			NEXT_ARG();
			ret = flower_parse_ip_proto(*argv, vlan_ethtype ?
						    vlan_ethtype : eth_type,
						    TCA_FLOWER_KEY_IP_PROTO,
						    &ip_proto, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"ip_proto\"\n");
				return -1;
			}
		} else if (matches(*argv, "dst_ip") == 0) {
			NEXT_ARG();
			ret = flower_parse_ip_addr(*argv, vlan_ethtype ?
						   vlan_ethtype : eth_type,
						   TCA_FLOWER_KEY_IPV4_DST,
						   TCA_FLOWER_KEY_IPV4_DST_MASK,
						   TCA_FLOWER_KEY_IPV6_DST,
						   TCA_FLOWER_KEY_IPV6_DST_MASK,
						   n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"dst_ip\"\n");
				return -1;
			}
		} else if (matches(*argv, "src_ip") == 0) {
			NEXT_ARG();
			ret = flower_parse_ip_addr(*argv, vlan_ethtype ?
						   vlan_ethtype : eth_type,
						   TCA_FLOWER_KEY_IPV4_SRC,
						   TCA_FLOWER_KEY_IPV4_SRC_MASK,
						   TCA_FLOWER_KEY_IPV6_SRC,
						   TCA_FLOWER_KEY_IPV6_SRC_MASK,
						   n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"src_ip\"\n");
				return -1;
			}
		} else if (matches(*argv, "dst_port") == 0) {
			NEXT_ARG();
			ret = flower_parse_port(*argv, ip_proto, false, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"dst_port\"\n");
				return -1;
			}
		} else if (matches(*argv, "src_port") == 0) {
			NEXT_ARG();
			ret = flower_parse_port(*argv, ip_proto, true, n);
			if (ret < 0) {
				fprintf(stderr, "Illegal \"src_port\"\n");
				return -1;
			}
		} else if (matches(*argv, "action") == 0) {
			NEXT_ARG();
			ret = parse_action(&argc, &argv, TCA_FLOWER_ACT, n);
			if (ret) {
				fprintf(stderr, "Illegal \"action\"\n");
				return -1;
			}
			continue;
		} else if (strcmp(*argv, "help") == 0) {
			explain();
			return -1;
		} else {
			fprintf(stderr, "What is \"%s\"?\n", *argv);
			explain();
			return -1;
		}
		argc--; argv++;
	}

parse_done:
	addattr32(n, MAX_MSG, TCA_FLOWER_FLAGS, flags);

	ret = addattr16(n, MAX_MSG, TCA_FLOWER_KEY_ETH_TYPE, eth_type);
	if (ret) {
		fprintf(stderr, "Illegal \"eth_type\"(0x%x)\n",
			ntohs(eth_type));
		return -1;
	}

	tail->rta_len = (((void *)n)+n->nlmsg_len) - (void *)tail;

	return 0;
}

static int __mask_bits(char *addr, size_t len)
{
	int bits = 0;
	bool hole = false;
	int i;
	int j;

	for (i = 0; i < len; i++, addr++) {
		for (j = 7; j >= 0; j--) {
			if (((*addr) >> j) & 0x1) {
				if (hole)
					return -1;
				bits++;
			} else if (bits) {
				hole = true;
			} else{
				return -1;
			}
		}
	}
	return bits;
}

static void flower_print_eth_addr(FILE *f, char *name,
				  struct rtattr *addr_attr,
				  struct rtattr *mask_attr)
{
	SPRINT_BUF(b1);
	int bits;

	if (!addr_attr || RTA_PAYLOAD(addr_attr) != ETH_ALEN)
		return;
	fprintf(f, "\n  %s %s", name, ll_addr_n2a(RTA_DATA(addr_attr), ETH_ALEN,
						  0, b1, sizeof(b1)));
	if (!mask_attr || RTA_PAYLOAD(mask_attr) != ETH_ALEN)
		return;
	bits = __mask_bits(RTA_DATA(mask_attr), ETH_ALEN);
	if (bits < 0)
		fprintf(f, "/%s", ll_addr_n2a(RTA_DATA(mask_attr), ETH_ALEN,
					      0, b1, sizeof(b1)));
	else if (bits < ETH_ALEN * 8)
		fprintf(f, "/%d", bits);
}

static void flower_print_eth_type(FILE *f, __be16 *p_eth_type,
				  struct rtattr *eth_type_attr)
{
	__be16 eth_type;

	if (!eth_type_attr)
		return;

	eth_type = rta_getattr_u16(eth_type_attr);
	fprintf(f, "\n  eth_type ");
	if (eth_type == htons(ETH_P_IP))
		fprintf(f, "ipv4");
	else if (eth_type == htons(ETH_P_IPV6))
		fprintf(f, "ipv6");
	else
		fprintf(f, "%04x", ntohs(eth_type));
	*p_eth_type = eth_type;
}

static void flower_print_ip_proto(FILE *f, __u8 *p_ip_proto,
				  struct rtattr *ip_proto_attr)
{
	__u8 ip_proto;

	if (!ip_proto_attr)
		return;

	ip_proto = rta_getattr_u8(ip_proto_attr);
	fprintf(f, "\n  ip_proto ");
	if (ip_proto == IPPROTO_TCP)
		fprintf(f, "tcp");
	else if (ip_proto == IPPROTO_UDP)
		fprintf(f, "udp");
	else if (ip_proto == IPPROTO_SCTP)
		fprintf(f, "sctp");
	else
		fprintf(f, "%02x", ip_proto);
	*p_ip_proto = ip_proto;
}

static void flower_print_ip_addr(FILE *f, char *name, __be16 eth_type,
				 struct rtattr *addr4_attr,
				 struct rtattr *mask4_attr,
				 struct rtattr *addr6_attr,
				 struct rtattr *mask6_attr)
{
	struct rtattr *addr_attr;
	struct rtattr *mask_attr;
	int family;
	size_t len;
	int bits;

	if (eth_type == htons(ETH_P_IP)) {
		family = AF_INET;
		addr_attr = addr4_attr;
		mask_attr = mask4_attr;
		len = 4;
	} else if (eth_type == htons(ETH_P_IPV6)) {
		family = AF_INET6;
		addr_attr = addr6_attr;
		mask_attr = mask6_attr;
		len = 16;
	} else {
		return;
	}
	if (!addr_attr || RTA_PAYLOAD(addr_attr) != len)
		return;
	fprintf(f, "\n  %s %s", name, rt_addr_n2a_rta(family, addr_attr));
	if (!mask_attr || RTA_PAYLOAD(mask_attr) != len)
		return;
	bits = __mask_bits(RTA_DATA(mask_attr), len);
	if (bits < 0)
		fprintf(f, "/%s", rt_addr_n2a_rta(family, mask_attr));
	else if (bits < len * 8)
		fprintf(f, "/%d", bits);
}

static void flower_print_port(FILE *f, char *name, struct rtattr *attr)
{
	fprintf(f, "\n  %s %d", name, rta_getattr_be16(attr));
}

static int flower_print_opt(struct filter_util *qu, FILE *f,
			    struct rtattr *opt, __u32 handle)
{
	struct rtattr *tb[TCA_FLOWER_MAX + 1];
	__be16 eth_type = 0;
	__u8 ip_proto = 0xff;

	if (!opt)
		return 0;

	parse_rtattr_nested(tb, TCA_FLOWER_MAX, opt);

	if (handle)
		fprintf(f, "handle 0x%x ", handle);

	if (tb[TCA_FLOWER_CLASSID]) {
		SPRINT_BUF(b1);
		fprintf(f, "classid %s ",
			sprint_tc_classid(rta_getattr_u32(tb[TCA_FLOWER_CLASSID]),
					  b1));
	}

	if (tb[TCA_FLOWER_INDEV]) {
		struct rtattr *attr = tb[TCA_FLOWER_INDEV];

		fprintf(f, "\n  indev %s", rta_getattr_str(attr));
	}

	if (tb[TCA_FLOWER_KEY_VLAN_ID]) {
		struct rtattr *attr = tb[TCA_FLOWER_KEY_VLAN_ID];

		fprintf(f, "\n  vlan_id %d", rta_getattr_u16(attr));
	}

	if (tb[TCA_FLOWER_KEY_VLAN_PRIO]) {
		struct rtattr *attr = tb[TCA_FLOWER_KEY_VLAN_PRIO];

		fprintf(f, "\n  vlan_prio %d", rta_getattr_u8(attr));
	}

	flower_print_eth_addr(f, "dst_mac", tb[TCA_FLOWER_KEY_ETH_DST],
			      tb[TCA_FLOWER_KEY_ETH_DST_MASK]);
	flower_print_eth_addr(f, "src_mac", tb[TCA_FLOWER_KEY_ETH_SRC],
			      tb[TCA_FLOWER_KEY_ETH_SRC_MASK]);

	flower_print_eth_type(f, &eth_type, tb[TCA_FLOWER_KEY_ETH_TYPE]);
	flower_print_ip_proto(f, &ip_proto, tb[TCA_FLOWER_KEY_IP_PROTO]);

	flower_print_ip_addr(f, "dst_ip", eth_type,
			     tb[TCA_FLOWER_KEY_IPV4_DST],
			     tb[TCA_FLOWER_KEY_IPV4_DST_MASK],
			     tb[TCA_FLOWER_KEY_IPV6_DST],
			     tb[TCA_FLOWER_KEY_IPV6_DST_MASK]);

	flower_print_ip_addr(f, "src_ip", eth_type,
			     tb[TCA_FLOWER_KEY_IPV4_SRC],
			     tb[TCA_FLOWER_KEY_IPV4_SRC_MASK],
			     tb[TCA_FLOWER_KEY_IPV6_SRC],
			     tb[TCA_FLOWER_KEY_IPV6_SRC_MASK]);

	flower_print_port(f, "dst_port",
			  tb[flower_port_attr_type(ip_proto, false)]);
	flower_print_port(f, "src_port",
			  tb[flower_port_attr_type(ip_proto, true)]);

	if (tb[TCA_FLOWER_FLAGS]) {
		__u32 flags = rta_getattr_u32(tb[TCA_FLOWER_FLAGS]);

		if (flags & TCA_CLS_FLAGS_SKIP_HW)
			fprintf(f, "\n  skip_hw");
		if (flags & TCA_CLS_FLAGS_SKIP_SW)
			fprintf(f, "\n  skip_sw");
	}

	if (tb[TCA_FLOWER_ACT])
		tc_print_action(f, tb[TCA_FLOWER_ACT]);

	return 0;
}

struct filter_util flower_filter_util = {
	.id = "flower",
	.parse_fopt = flower_parse_opt,
	.print_fopt = flower_print_opt,
};
