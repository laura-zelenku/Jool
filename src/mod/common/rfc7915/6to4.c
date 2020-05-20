#include "mod/common/rfc7915/6to4.h"

#include <linux/inetdevice.h>
#include <net/ip6_checksum.h>

#include "mod/common/ipv6_hdr_iterator.h"
#include "mod/common/linux_version.h"
#include "mod/common/log.h"
#include "mod/common/route.h"
#include "mod/common/steps/compute_outgoing_tuple.h"

static __u8 xlat_tos(struct jool_globals const *config, struct ipv6hdr const *hdr)
{
	return config->reset_tos ? config->new_tos : get_traffic_class(hdr);
}

/**
 * One-liner for creating the IPv4 header's Protocol field.
 */
static __u8 xlat_proto(struct ipv6hdr const *hdr6)
{
	struct hdr_iterator iterator = HDR_ITERATOR_INIT(hdr6);
	hdr_iterator_last(&iterator);
	return (iterator.hdr_type == NEXTHDR_ICMP)
			? IPPROTO_ICMP
			: iterator.hdr_type;
}

static verdict xlat_external_addresses(struct xlation *state, union flowix *flowx)
{
	switch (xlator_get_type(&state->jool)) {
	case XT_NAT64:
		flowx->v4.flowi.saddr = state->out.tuple.src.addr4.l3.s_addr;
		flowx->v4.flowi.daddr = state->out.tuple.dst.addr4.l3.s_addr;
		return VERDICT_CONTINUE;

	case XT_SIIT:
		return translate_addrs64_siit(state,
				&flowx->v4.flowi.saddr,
				&flowx->v4.flowi.daddr);
	}

	WARN(1, "xlator type is not SIIT nor NAT64: %u",
			xlator_get_type(&state->jool));
	return drop(state, JSTAT_UNKNOWN);
}

static verdict xlat_internal_addresses(struct xlation *state,
		union flowix *flowx)
{
	struct bkp_skb_tuple bkp;
	verdict result;

	switch (xlator_get_type(&state->jool)) {
	case XT_NAT64:
		flowx->v4.inner_src = state->out.tuple.dst.addr4.l3;
		flowx->v4.inner_dst = state->out.tuple.src.addr4.l3;
		return VERDICT_CONTINUE;

	case XT_SIIT:
		result = become_inner_packet(state, &bkp, false);
		if (result != VERDICT_CONTINUE)
			return result;
		log_debug("Translating internal addresses...");
		result = translate_addrs64_siit(state,
				&flowx->v4.inner_src.s_addr,
				&flowx->v4.inner_dst.s_addr);
		restore_outer_packet(state, &bkp, false);
		return result;
	}

	WARN(1, "xlator type is not SIIT nor NAT64: %u",
			xlator_get_type(&state->jool));
	return drop(state, JSTAT_UNKNOWN);
}

static verdict xlat_tcp_ports(struct xlation const *state,
		struct flowi4 *flowi)
{
	struct tcphdr const *hdr;

	switch (xlator_get_type(&state->jool)) {
	case XT_NAT64:
		flowi->fl4_sport = state->out.tuple.src.addr4.l4;
		flowi->fl4_dport = state->out.tuple.dst.addr4.l4;
		break;
	case XT_SIIT:
		hdr = pkt_tcp_hdr(&state->in);
		flowi->fl4_sport = hdr->source;
		flowi->fl4_dport = hdr->dest;
	}

	return VERDICT_CONTINUE;
}

static verdict xlat_udp_ports(struct xlation const *state,
		struct flowi4 *flowi)
{
	struct udphdr const *udp;

	switch (xlator_get_type(&state->jool)) {
	case XT_NAT64:
		flowi->fl4_sport = state->out.tuple.src.addr4.l4;
		flowi->fl4_dport = state->out.tuple.dst.addr4.l4;
		break;
	case XT_SIIT:
		udp = pkt_udp_hdr(&state->in);
		flowi->fl4_sport = udp->source;
		flowi->fl4_dport = udp->dest;
	}

	return VERDICT_CONTINUE;
}

static verdict xlat_icmp_type(struct xlation *state, union flowix *flowx)
{
	struct icmp6hdr const *hdr;
	struct flowi4 *flow4;

	hdr = pkt_icmp6_hdr(&state->in);
	flow4 = &flowx->v4.flowi;

	switch (hdr->icmp6_type) {
	case ICMPV6_ECHO_REQUEST:
		flow4->fl4_icmp_type = ICMP_ECHO;
		flow4->fl4_icmp_code = 0;
		return VERDICT_CONTINUE;

	case ICMPV6_ECHO_REPLY:
		flow4->fl4_icmp_type = ICMP_ECHOREPLY;
		flow4->fl4_icmp_code = 0;
		return VERDICT_CONTINUE;

	case ICMPV6_DEST_UNREACH:
		flow4->fl4_icmp_type = ICMP_DEST_UNREACH;
		switch (hdr->icmp6_code) {
		case ICMPV6_NOROUTE:
		case ICMPV6_NOT_NEIGHBOUR:
		case ICMPV6_ADDR_UNREACH:
			flow4->fl4_icmp_code = ICMP_HOST_UNREACH;
			return xlat_internal_addresses(state, flowx);
		case ICMPV6_ADM_PROHIBITED:
			flow4->fl4_icmp_code = ICMP_HOST_ANO;
			return xlat_internal_addresses(state, flowx);
		case ICMPV6_PORT_UNREACH:
			flow4->fl4_icmp_code = ICMP_PORT_UNREACH;
			return xlat_internal_addresses(state, flowx);
		}
		break;

	case ICMPV6_PKT_TOOBIG:
		flow4->fl4_icmp_type = ICMP_DEST_UNREACH;
		flow4->fl4_icmp_code = ICMP_FRAG_NEEDED;
		return xlat_internal_addresses(state, flowx);

	case ICMPV6_TIME_EXCEED:
		flow4->fl4_icmp_type = ICMP_TIME_EXCEEDED;
		flow4->fl4_icmp_code = hdr->icmp6_code;
		return xlat_internal_addresses(state, flowx);

	case ICMPV6_PARAMPROB:
		switch (hdr->icmp6_code) {
		case ICMPV6_HDR_FIELD:
			flow4->fl4_icmp_type = ICMP_PARAMETERPROB;
			flow4->fl4_icmp_code = 0;
			return xlat_internal_addresses(state, flowx);
		case ICMPV6_UNK_NEXTHDR:
			flow4->fl4_icmp_type = ICMP_DEST_UNREACH;
			flow4->fl4_icmp_code = ICMP_PROT_UNREACH;
			return xlat_internal_addresses(state, flowx);
		}
	}

	/*
	 * The following codes are known to fall through here:
	 * ICMPV6_MGM_QUERY, ICMPV6_MGM_REPORT, ICMPV6_MGM_REDUCTION, Neighbor
	 * Discover messages (133 - 137).
	 */
	log_debug("ICMPv6 messages type %u code %u lack an ICMPv4 counterpart.",
			hdr->icmp6_type, hdr->icmp6_code);
	return drop(state, JSTAT_UNKNOWN_ICMP6_TYPE);
}

static verdict compute_flowix64(struct xlation *state, union flowix *flowx)
{
	struct flowi4 *flow4;
	struct ipv6hdr const *hdr6;
	verdict result;

	memset(&flowx->v4, 0, sizeof(flowx->v4));
	flow4 = &flowx->v4.flowi;
	hdr6 = pkt_ip6_hdr(&state->in);

	flow4->flowi4_mark = state->in.skb->mark;
	flow4->flowi4_tos = xlat_tos(&state->jool.globals, hdr6);
	flow4->flowi4_scope = RT_SCOPE_UNIVERSE;
	flow4->flowi4_proto = xlat_proto(hdr6);
	/*
	 * ANYSRC disables the source address reachable validation.
	 * It's best to include it because none of the xlat addresses are
	 * required to be present in the routing table.
	 */
	flow4->flowi4_flags = FLOWI_FLAG_ANYSRC;

	result = xlat_external_addresses(state, flowx);
	if (result != VERDICT_CONTINUE)
		return result;

	switch (flow4->flowi4_proto) {
	case IPPROTO_TCP:
		return xlat_tcp_ports(state, flow4);
	case IPPROTO_UDP:
		return xlat_udp_ports(state, flow4);
	case IPPROTO_ICMP:
		return xlat_icmp_type(state, flowx);
	}

	return VERDICT_CONTINUE;
}

static verdict select_good_saddr(struct xlation *state, union flowix *flowx,
		struct dst_entry const *dst)
{
	flowx->v4.flowi.saddr = inet_select_addr(dst->dev,
			flowx->v4.flowi.daddr, RT_SCOPE_UNIVERSE);
	if (flowx->v4.flowi.saddr == 0) {
		log_debug("ICMPv6 error has untranslatable source, but the kernel could not find a suitable source for destination %pI4.",
				&flowx->v4.flowi.daddr);
		return drop(state, JSTAT64_6791_ENOENT);
	}

	return VERDICT_CONTINUE;
}

static verdict select_any_saddr(struct xlation *state, union flowix *flowx)
{
	struct net_device *dev;
	struct in_device *in_dev;

	rcu_read_lock();
	for_each_netdev_rcu(state->jool.ns, dev) {
		in_dev = __in_dev_get_rcu(dev);
		if (!in_dev)
			continue;

		for_primary_ifa(in_dev) {
			if (ifa->ifa_scope == RT_SCOPE_UNIVERSE) {
				flowx->v4.flowi.saddr = ifa->ifa_local;
				rcu_read_unlock();
				return VERDICT_CONTINUE;
			}
		} endfor_ifa(in_dev);
	}

	rcu_read_unlock();
	log_debug("ICMPv6 error has untranslatable source, and there aren't any universe-scoped addresses to mask it with.");
	return drop(state, JSTAT64_6791_ENOENT);
}

/**
 * Please note: @result might be NULL even on VERDICT_CONTINUE. Handle properly.
 */
static verdict predict_route64(struct xlation *state, union flowix *flowx,
		struct dst_entry **__dst)
{
	struct dst_entry *dst;
	verdict result;

	*__dst = NULL;
#ifdef UNIT_TESTING
	return VERDICT_CONTINUE;
#endif

	if (state->is_hairpin) {
		log_debug("Packet is hairpinning; skipping routing.");
		dst = NULL;
	} else {
		log_debug("Routing: %pI4->%pI4", &flowx->v4.flowi.saddr,
				&flowx->v4.flowi.daddr);
		dst = route4(state->jool.ns, &flowx->v4.flowi);
		if (!dst)
			return untranslatable(state, JSTAT_FAILED_ROUTES);
	}

	if (flowx->v4.flowi.saddr == 0) { /* Empty pool4 or empty pool6791v4 */
		if (dst) {
			result = select_good_saddr(state, flowx, dst);
			if (result != VERDICT_CONTINUE) {
				dst_release(dst);
				return result;
			}
		} else {
			result = select_any_saddr(state, flowx);
			if (result != VERDICT_CONTINUE)
				return result;
		}
	}

	*__dst = dst;
	return VERDICT_CONTINUE;
}

static int fragment_exceeds_mtu64(struct packet const *in, unsigned int mtu)
{
	struct sk_buff *iter;
	int delta;

	delta = sizeof(struct iphdr) - pkt_l3hdr_len(in);

	if (!skb_shinfo(in->skb)->frag_list) {
		if (in->skb->len + delta > mtu)
			return is_first_frag6(pkt_frag_hdr(in)) ? 1 : 2;
		return 0;
	}

	if (skb_headlen(in->skb) + delta > mtu)
		return 1;

	mtu -= sizeof(struct iphdr);
	skb_walk_frags(in->skb, iter)
		if (iter->len > mtu)
			return 2;

	return 0;
}

static verdict validate_size(struct xlation *state, struct dst_entry const *dst)
{
	unsigned int nexthop_mtu;

	if (!dst || is_icmp6_error(pkt_icmp6_hdr(&state->in)->icmp6_type))
		return VERDICT_CONTINUE;

	nexthop_mtu = dst_mtu(dst);
	switch (fragment_exceeds_mtu64(&state->in, nexthop_mtu)) {
	case 0:
		return VERDICT_CONTINUE;
	case 1:
		return drop_icmp(state, JSTAT_PKT_TOO_BIG, ICMPERR_FRAG_NEEDED,
				max(1280u, nexthop_mtu + 20u));
	case 2:
		return drop(state, JSTAT_PKT_TOO_BIG);
	}

	WARN(1, "fragment_exceeds_mtu64() returned garbage.");
	return drop(state, JSTAT_UNKNOWN);
}

static verdict ttp64_alloc_skb(struct xlation *state, union flowix *flowx)
{
	struct packet const *in = &state->in;
	struct sk_buff *out;
	struct skb_shared_info *shinfo;
	struct dst_entry *dst;
	verdict result;

	result = compute_flowix64(state, flowx);
	if (result != VERDICT_CONTINUE)
		return result;
	result = predict_route64(state, flowx, &dst);
	if (result != VERDICT_CONTINUE)
		return result;
	result = validate_size(state, dst);
	if (result != VERDICT_CONTINUE)
		goto revert;

	/*
	 * I'm going to use __pskb_copy() (via pskb_copy()) because I need the
	 * incoming and outgoing packets to share the same paged data. This is
	 * not only for the sake of performance (prevents lots of data copying
	 * and large contiguous skbs in memory) but also because the pages need
	 * to survive the translation for GSO to work.
	 *
	 * Since the IPv4 version of the packet is going to be invariably
	 * smaller than its IPv6 counterpart, you'd think we should reserve less
	 * memory for it. But there's a problem: __pskb_copy() only allows us to
	 * shrink the headroom; not the head. If we try to shrink the head
	 * through the headroom and the v6 packet happens to have one too many
	 * extension headers, the `headroom` we'll send to __pskb_copy() will be
	 * negative, and then skb_copy_from_linear_data() will write onto the
	 * tail area without knowing it. (I'm reading the Linux 4.4 code.)
	 *
	 * We will therefore *not* attempt to allocate less.
	 */

	out = pskb_copy(in->skb, GFP_ATOMIC);
	if (!out) {
		log_debug("pskb_copy() returned NULL.");
		result = drop(state, JSTAT64_PSKB_COPY);
		goto revert;
	}

	/* https://github.com/NICMx/Jool/issues/289 */
#if LINUX_VERSION_AT_LEAST(5, 4, 0, 9999, 0)
	nf_reset_ct(out);
#else
	nf_reset(out);
#endif

	/* Remove outer l3 and l4 headers from the copy. */
	skb_pull(out, pkt_hdrs_len(in));

	if (is_first_frag6(pkt_frag_hdr(in)) && pkt_is_icmp6_error(in)) {
		struct ipv6hdr *hdr = pkt_payload(in);
		struct hdr_iterator iterator = HDR_ITERATOR_INIT(hdr);
		hdr_iterator_last(&iterator);

		/* Remove inner l3 headers from the copy. */
		skb_pull(out, iterator.data - (void *)hdr);

		/* Add inner l3 headers to the copy. */
		skb_push(out, sizeof(struct iphdr));
	}

	/* Add outer l4 headers to the copy. */
	skb_push(out, pkt_l4hdr_len(in));
	/* Add outer l3 headers to the copy. */
	skb_push(out, sizeof(struct iphdr));

	skb_reset_mac_header(out);
	skb_reset_network_header(out);
	skb_set_transport_header(out, sizeof(struct iphdr));

	/* Wrap up. */
	pkt_fill(&state->out, out, L3PROTO_IPV4, pkt_l4_proto(in),
			NULL, skb_transport_header(out) + pkt_l4hdr_len(in),
			pkt_original_pkt(in));

	memset(out->cb, 0, sizeof(out->cb));
	out->mark = flowx->v4.flowi.flowi4_mark;
	out->protocol = htons(ETH_P_IP);

	shinfo = skb_shinfo(out);
	if (shinfo->gso_type & SKB_GSO_TCPV6) {
		shinfo->gso_type &= ~SKB_GSO_TCPV6;
		shinfo->gso_type |= SKB_GSO_TCPV4;
	}

	if (dst)
		skb_dst_set(out, dst);
	return VERDICT_CONTINUE;

revert:
	if (dst)
		dst_release(dst);
	return result;
}

/**
 * One-liner for creating the IPv4 header's Identification field.
 */
static void generate_ipv4_id(struct xlation const *state, struct iphdr *hdr4,
    struct frag_hdr const *hdr_frag)
{
	if (hdr_frag) {
		hdr4->id = cpu_to_be16(be32_to_cpu(hdr_frag->identification));
	} else {
#if LINUX_VERSION_AT_LEAST(4, 1, 0, 7, 3)
		__ip_select_ident(state->jool.ns, hdr4, 1);
#else
		__ip_select_ident(hdr4, 1);
#endif
	}
}

/**
 * One-liner for creating the IPv4 header's Dont Fragment flag.
 */
static bool generate_df_flag(struct packet const *out)
{
	unsigned int len;

	len = pkt_is_outer(out)
			? pkt_len(out)
			: be16_to_cpu(pkt_ip4_hdr(out)->tot_len);

	return len > 1260;
}

static __be16 xlat_frag_off(struct frag_hdr const *hdr_frag, struct packet const *out)
{
	bool df;
	__u16 mf;
	__u16 frag_offset;

	if (hdr_frag) {
		df = 0;
		mf = is_mf_set_ipv6(hdr_frag);
		frag_offset = get_fragment_offset_ipv6(hdr_frag);
	} else {
		df = generate_df_flag(out);
		mf = 0;
		frag_offset = 0;
	}

	return build_ipv4_frag_off_field(df, mf, frag_offset);
}

/**
 * has_nonzero_segments_left - Returns true if @hdr6's packet has a routing
 * header, and its Segments Left field is not zero.
 *
 * @location: if the packet has nonzero segments left, the offset
 *		of the segments left field (from the start of @hdr6) will be
 *		stored here.
 */
static bool has_nonzero_segments_left(struct ipv6hdr const *hdr6,
		__u32 *location)
{
	struct ipv6_rt_hdr const *rt_hdr;
	unsigned int offset;

	rt_hdr = hdr_iterator_find(hdr6, NEXTHDR_ROUTING);
	if (!rt_hdr)
		return false;

	if (rt_hdr->segments_left == 0)
		return false;

	offset = ((void *)rt_hdr) - (void *)hdr6;
	*location = offset + offsetof(struct ipv6_rt_hdr, segments_left);
	return true;
}

/**
 * Translates @state->in's IPv6 header into @state->out's IPv4 header.
 * Only used for external IPv6 headers. (ie. not enclosed in ICMP errors.)
 * RFC 7915 sections 5.1 and 5.1.1.
 */
static verdict ttp64_ipv4_external(struct xlation *state,
		union flowix const *flowx)
{
	struct packet const *in = &state->in;
	struct packet *out = &state->out;
	struct ipv6hdr const *hdr6 = pkt_ip6_hdr(in);
	struct iphdr *hdr4 = pkt_ip4_hdr(out);
	struct frag_hdr const *hdr_frag = pkt_frag_hdr(in);
	__u32 nonzero_location;

	if (hdr6->hop_limit <= 1) {
		log_debug("Packet's hop limit <= 1.");
		return drop_icmp(state, JSTAT64_TTL, ICMPERR_TTL, 0);
	}
	if (has_nonzero_segments_left(hdr6, &nonzero_location)) {
		log_debug("Packet's segments left field is nonzero.");
		return drop_icmp(state, JSTAT64_SEGMENTS_LEFT,
				ICMPERR_HDR_FIELD, nonzero_location);
	}

	hdr4->version = 4;
	hdr4->ihl = 5;
	hdr4->tos = flowx->v4.flowi.flowi4_tos;
	hdr4->tot_len = cpu_to_be16(out->skb->len);
	generate_ipv4_id(state, hdr4, hdr_frag);
	hdr4->frag_off = xlat_frag_off(hdr_frag, out);
	hdr4->ttl = hdr6->hop_limit - 1;
	hdr4->protocol = flowx->v4.flowi.flowi4_proto;
	/* ip4_hdr->check is set later; please scroll down. */
	hdr4->saddr = flowx->v4.flowi.saddr;
	hdr4->daddr = flowx->v4.flowi.daddr;
	hdr4->check = 0;
	hdr4->check = ip_fast_csum(hdr4, hdr4->ihl);

	return VERDICT_CONTINUE;
}

/**
 * Same as ttp64_ipv4_external(), except only used on internal headers.
 */
static verdict ttp64_ipv4_internal(struct xlation *state,
		union flowix const *flowx)
{
	struct packet const *in = &state->in;
	struct packet *out = &state->out;
	struct ipv6hdr const *hdr6 = pkt_ip6_hdr(in);
	struct iphdr *hdr4 = pkt_ip4_hdr(out);
	struct frag_hdr const *hdr_frag = pkt_frag_hdr(in);

	hdr4->version = 4;
	hdr4->ihl = 5;
	hdr4->tos = xlat_tos(&state->jool.globals, hdr6);
	hdr4->tot_len = cpu_to_be16(get_tot_len_ipv6(in->skb) - pkt_hdrs_len(in)
			+ pkt_hdrs_len(out));
	generate_ipv4_id(state, hdr4, hdr_frag);
	hdr4->frag_off = xlat_frag_off(hdr_frag, out);
	hdr4->ttl = hdr6->hop_limit;
	hdr4->protocol = xlat_proto(hdr6);
	hdr4->saddr = flowx->v4.inner_src.s_addr;
	hdr4->daddr = flowx->v4.inner_dst.s_addr;
	hdr4->check = 0;
	hdr4->check = ip_fast_csum(hdr4, hdr4->ihl);

	return VERDICT_CONTINUE;
}

/**
 * One liner for creating the ICMPv4 header's MTU field.
 * Returns the smallest out of the three parameters.
 */
static __be16 minimum(unsigned int mtu1, unsigned int mtu2, unsigned int mtu3)
{
	return cpu_to_be16(min(mtu1, min(mtu2, mtu3)));
}

static verdict compute_mtu4(struct xlation const *state)
{
	/* Meant for unit tests. */
	static const unsigned int INFINITE = 0xffffffff;
	struct icmphdr *out_icmp;
	struct icmp6hdr const *in_icmp;
	struct net_device const *in_dev;
	struct dst_entry const *out_dst;
	unsigned int in_mtu;
	unsigned int out_mtu;

	out_icmp = pkt_icmp4_hdr(&state->out);
	in_icmp = pkt_icmp6_hdr(&state->in);
	in_dev = state->in.skb->dev;
	in_mtu = in_dev ? in_dev->mtu : INFINITE;
	out_dst = skb_dst(state->out.skb);
	out_mtu = out_dst ? dst_mtu(out_dst) : INFINITE;

	log_debug("Packet MTU: %u", be32_to_cpu(in_icmp->icmp6_mtu));
	log_debug("In dev MTU: %u", in_mtu);
	log_debug("Out dev MTU: %u", out_mtu);

	out_icmp->un.frag.mtu = minimum(be32_to_cpu(in_icmp->icmp6_mtu) - 20,
			out_mtu,
			in_mtu - 20);
	log_debug("Resulting MTU: %u", be16_to_cpu(out_icmp->un.frag.mtu));

	return VERDICT_CONTINUE;
}

/**
 * One liner for translating the ICMPv6's pointer field to ICMPv4.
 * "Pointer" is a field from "Parameter Problem" ICMP messages.
 */
static verdict icmp6_to_icmp4_param_prob_ptr(struct xlation *state)
{
	struct icmp6hdr const *icmpv6_hdr = pkt_icmp6_hdr(&state->in);
	struct icmphdr *icmpv4_hdr = pkt_icmp4_hdr(&state->out);
	__u32 icmp6_ptr = be32_to_cpu(icmpv6_hdr->icmp6_dataun.un_data32[0]);
	__u32 icmp4_ptr;

	if (icmp6_ptr < 0 || 39 < icmp6_ptr)
		goto failure;

	switch (icmp6_ptr) {
	case 0:
		icmp4_ptr = 0;
		goto success;
	case 1:
		icmp4_ptr = 1;
		goto success;
	case 2:
	case 3:
		goto failure;
	case 4:
	case 5:
		icmp4_ptr = 2;
		goto success;
	case 6:
		icmp4_ptr = 9;
		goto success;
	case 7:
		icmp4_ptr = 8;
		goto success;
	}

	if (icmp6_ptr >= 24) {
		icmp4_ptr = 16;
		goto success;
	}
	if (icmp6_ptr >= 8) {
		icmp4_ptr = 12;
		goto success;
	}

	/* The above ifs are supposed to cover all the possible values. */
	WARN(true, "Parameter problem pointer '%u' is unknown.", icmp6_ptr);
	goto failure;

success:
	icmpv4_hdr->icmp4_unused = cpu_to_be32(icmp4_ptr << 24);
	return VERDICT_CONTINUE;
failure:
	log_debug("Parameter problem pointer '%u' lacks an ICMPv4 counterpart.",
			icmp6_ptr);
	return drop(state, JSTAT64_UNTRANSLATABLE_PARAM_PROB_PTR);
}

/**
 * One-liner for translating "Parameter Problem" messages from ICMPv6 to ICMPv4.
 */
static verdict icmp6_to_icmp4_param_prob(struct xlation *state)
{
	struct icmp6hdr const *icmpv6_hdr = pkt_icmp6_hdr(&state->in);
	struct icmphdr *icmpv4_hdr = pkt_icmp4_hdr(&state->out);

	switch (icmpv6_hdr->icmp6_code) {
	case ICMPV6_HDR_FIELD:
		return icmp6_to_icmp4_param_prob_ptr(state);

	case ICMPV6_UNK_NEXTHDR:
		icmpv4_hdr->icmp4_unused = 0;
		return VERDICT_CONTINUE;
	}

	/* Dead code */
	WARN(1, "ICMPv6 Parameter Problem code %u was unhandled by the switch above.",
			icmpv6_hdr->icmp6_type);
	return drop(state, JSTAT_UNKNOWN);
}

/*
 * Use this when only the ICMP header changed, so all there is to do is subtract
 * the old data from the checksum and add the new one.
 */
static void update_icmp4_csum(struct xlation const *state)
{
	struct ipv6hdr const *in_ip6 = pkt_ip6_hdr(&state->in);
	struct icmp6hdr const *in_icmp = pkt_icmp6_hdr(&state->in);
	struct icmphdr *out_icmp = pkt_icmp4_hdr(&state->out);
	struct icmp6hdr copy_hdr;
	__wsum csum, tmp;

	csum = ~csum_unfold(in_icmp->icmp6_cksum);

	/* Remove the ICMPv6 pseudo-header. */
	tmp = ~csum_unfold(csum_ipv6_magic(&in_ip6->saddr, &in_ip6->daddr,
			pkt_datagram_len(&state->in), NEXTHDR_ICMP, 0));
	csum = csum_sub(csum, tmp);

	/*
	 * Remove the ICMPv6 header.
	 * I'm working on a copy because I need to zero out its checksum.
	 * If I did that directly on the skb, I'd need to make it writable
	 * first.
	 */
	memcpy(&copy_hdr, in_icmp, sizeof(*in_icmp));
	copy_hdr.icmp6_cksum = 0;
	tmp = csum_partial(&copy_hdr, sizeof(copy_hdr), 0);
	csum = csum_sub(csum, tmp);

	/* Add the ICMPv4 header. There's no ICMPv4 pseudo-header. */
	out_icmp->checksum = 0;
	tmp = csum_partial(out_icmp, sizeof(*out_icmp), 0);
	csum = csum_add(csum, tmp);

	out_icmp->checksum = csum_fold(csum);
}

/**
 * Use this when header and payload both changed completely, so we gotta just
 * trash the old checksum and start anew.
 */
static void compute_icmp4_csum(struct packet const *out)
{
	struct icmphdr *hdr = pkt_icmp4_hdr(out);

	/*
	 * This function only gets called for ICMP error checksums, so
	 * pkt_datagram_len() is fine.
	 */
	hdr->checksum = 0;
	hdr->checksum = csum_fold(skb_checksum(out->skb,
			skb_transport_offset(out->skb),
			pkt_datagram_len(out), 0));
	out->skb->ip_summed = CHECKSUM_NONE;
}

static verdict validate_icmp6_csum(struct xlation *state)
{
	struct packet const *in = &state->in;
	struct ipv6hdr const *hdr6;
	unsigned int len;
	__sum16 csum;

	if (in->skb->ip_summed != CHECKSUM_NONE)
		return VERDICT_CONTINUE;

	hdr6 = pkt_ip6_hdr(in);
	len = pkt_datagram_len(in);
	csum = csum_ipv6_magic(&hdr6->saddr, &hdr6->daddr, len, NEXTHDR_ICMP,
			skb_checksum(in->skb, skb_transport_offset(in->skb),
					len, 0));
	if (csum != 0) {
		log_debug("Checksum doesn't match.");
		return drop(state, JSTAT64_ICMP_CSUM);
	}

	return VERDICT_CONTINUE;
}

static void update_total_length(struct packet const *out)
{
	struct iphdr *hdr;
	unsigned int new_len;

	hdr = pkt_ip4_hdr(out);
	new_len = out->skb->len;

	if (be16_to_cpu(hdr->tot_len) == new_len)
		return;

	hdr->tot_len = cpu_to_be16(new_len);
	hdr->frag_off &= cpu_to_be16(~IP_DF); /* Assumes new_len <= 1260 */
	hdr->check = 0;
	hdr->check = ip_fast_csum(hdr, hdr->ihl);
}

static verdict handle_icmp4_extension(struct xlation *state)
{
	struct icmpext_args args;
	verdict result;
	struct packet *out;

	args.max_pkt_len = 576;
	args.ipl = pkt_icmp6_hdr(&state->in)->icmp6_length << 3;
	args.out_bits = 2;
	args.force_remove_ie = false;

	result = handle_icmp_extension(state, &args);
	if (result != VERDICT_CONTINUE)
		return result;

	out = &state->out;
	pkt_icmp4_hdr(out)->icmp4_length = args.ipl;
	update_total_length(out);
	return VERDICT_CONTINUE;
}

/*
 * According to my tests, if we send an ICMP error that exceeds the MTU, Linux
 * will either drop it (if skb->local_df is false) or fragment it (if
 * skb->local_df is true).
 * Neither of these possibilities is even remotely acceptable.
 * We'll maximize delivery probability by truncating to mandatory minimum size.
 */
static verdict trim_576(struct xlation *state)
{
	struct packet *out;
	int error;

	out = &state->out;
	if (out->skb->len <= 576)
		return VERDICT_CONTINUE;

	error = pskb_trim(out->skb, 576);
	if (error) {
		log_debug("pskb_trim() error: %d", error);
		return drop(state, JSTAT_ENOMEM);
	}

	update_total_length(out);
	return VERDICT_CONTINUE;
}

static verdict post_icmp4error(struct xlation *state, union flowix const *flowx,
		bool handle_extensions)
{
	verdict result;

	log_debug("Translating the inner packet (6->4)...");

	result = validate_icmp6_csum(state);
	if (result != VERDICT_CONTINUE)
		return result;

	result = ttpcomm_translate_inner_packet(state, flowx, &ttp64_steps);
	if (result != VERDICT_CONTINUE)
		return result;

	if (handle_extensions) {
		result = handle_icmp4_extension(state);
		if (result != VERDICT_CONTINUE)
			return result;
	}

	result = trim_576(state);
	if (result != VERDICT_CONTINUE)
		return result;

	compute_icmp4_csum(&state->out);
	return VERDICT_CONTINUE;
}

/**
 * Translates in's icmp6 header and payload into out's icmp4 header and payload.
 * This is the core of RFC 7915 sections 5.2 and 5.3, except checksum (See
 * post_icmp4*()).
 */
static verdict ttp64_icmp(struct xlation *state, union flowix const *flowx)
{
	struct icmp6hdr const *icmpv6_hdr = pkt_icmp6_hdr(&state->in);
	struct icmphdr *icmpv4_hdr = pkt_icmp4_hdr(&state->out);
	verdict result;

	icmpv4_hdr->type = flowx->v4.flowi.fl4_icmp_type;
	icmpv4_hdr->code = flowx->v4.flowi.fl4_icmp_code;
	icmpv4_hdr->checksum = icmpv6_hdr->icmp6_cksum; /* default. */

	switch (icmpv6_hdr->icmp6_type) {
	case ICMPV6_ECHO_REQUEST:
	case ICMPV6_ECHO_REPLY:
		icmpv4_hdr->un.echo.id = xlation_is_nat64(state)
				? cpu_to_be16(state->out.tuple.icmp4_id)
				: icmpv6_hdr->icmp6_identifier;
		icmpv4_hdr->un.echo.sequence = icmpv6_hdr->icmp6_sequence;
		update_icmp4_csum(state);
		return VERDICT_CONTINUE;

	case ICMPV6_DEST_UNREACH:
	case ICMPV6_TIME_EXCEED:
		icmpv4_hdr->icmp4_unused = 0;
		return post_icmp4error(state, flowx, true);

	case ICMPV6_PKT_TOOBIG:
		/*
		 * BTW, I have no idea what the RFC means by "taking into
		 * account whether or not the packet in error includes a
		 * Fragment Header"... What does the fragment header have to do
		 * with anything here?
		 */
		icmpv4_hdr->un.frag.__unused = htons(0);
		result = compute_mtu4(state);
		if (result != VERDICT_CONTINUE)
			return result;
		return post_icmp4error(state, flowx, false);

	case ICMPV6_PARAMPROB:
		result = icmp6_to_icmp4_param_prob(state);
		if (result != VERDICT_CONTINUE)
			return result;
		return post_icmp4error(state, flowx, false);
	}

	/* Dead code */
	WARN(1, "ICMPv6 type %u was unhandled by the switch above.",
			icmpv6_hdr->icmp6_type);
	return drop(state, JSTAT_UNKNOWN);
}

static __be16 get_src_port(struct packet const *pkt, union flowix const *flowx)
{
	return pkt_is_inner(pkt)
			? cpu_to_be16(pkt->tuple.dst.addr4.l4)
			: flowx->v4.flowi.fl4_sport;
}

static __be16 get_dst_port(struct packet const *pkt, union flowix const *flowx)
{
	return pkt_is_inner(pkt)
			? cpu_to_be16(pkt->tuple.src.addr4.l4)
			: flowx->v4.flowi.fl4_dport;
}

static __wsum pseudohdr6_csum(struct ipv6hdr const *hdr)
{
	return ~csum_unfold(csum_ipv6_magic(&hdr->saddr, &hdr->daddr, 0, 0, 0));
}

static __wsum pseudohdr4_csum(struct iphdr const *hdr)
{
	return csum_tcpudp_nofold(hdr->saddr, hdr->daddr, 0, 0, 0);
}

static __sum16 update_csum_6to4(__sum16 csum16,
		struct ipv6hdr const *in_ip6, void const *in_l4_hdr, size_t in_l4_hdr_len,
		struct iphdr const *out_ip4, void const *out_l4_hdr, size_t out_l4_hdr_len)
{
	__wsum csum;

	csum = ~csum_unfold(csum16);

	/*
	 * Regarding the pseudoheaders:
	 * The length is pretty hard to obtain if there's TCP and fragmentation,
	 * and whatever it is, it's not going to change. Therefore, instead of
	 * computing it only to cancel it out with itself later, simply sum
	 * (and substract) zero.
	 * Do the same with proto since we're feeling ballsy.
	 */

	/* Remove the IPv6 crap. */
	csum = csum_sub(csum, pseudohdr6_csum(in_ip6));
	csum = csum_sub(csum, csum_partial(in_l4_hdr, in_l4_hdr_len, 0));

	/* Add the IPv4 crap. */
	csum = csum_add(csum, pseudohdr4_csum(out_ip4));
	csum = csum_add(csum, csum_partial(out_l4_hdr, out_l4_hdr_len, 0));

	return csum_fold(csum);
}

static __sum16 update_csum_6to4_partial(__sum16 csum16, struct ipv6hdr const *in_ip6,
		struct iphdr *out_ip4)
{
	__wsum csum = csum_unfold(csum16);
	csum = csum_sub(csum, pseudohdr6_csum(in_ip6));
	csum = csum_add(csum, pseudohdr4_csum(out_ip4));
	return ~csum_fold(csum);
}

static verdict ttp64_tcp(struct xlation *state, union flowix const *flowx)
{
	struct packet const *in = &state->in;
	struct packet *out = &state->out;
	struct tcphdr const *tcp_in = pkt_tcp_hdr(in);
	struct tcphdr *tcp_out = pkt_tcp_hdr(out);
	struct tcphdr tcp_copy;

	/* Header */
	memcpy(tcp_out, tcp_in, pkt_l4hdr_len(in));
	if (xlation_is_nat64(state)) {
		tcp_out->source = get_src_port(out, flowx);
		tcp_out->dest = get_dst_port(out, flowx);
	}

	/* Header.checksum */
	if (in->skb->ip_summed != CHECKSUM_PARTIAL) {
		memcpy(&tcp_copy, tcp_in, sizeof(*tcp_in));
		tcp_copy.check = 0;

		tcp_out->check = 0;
		tcp_out->check = update_csum_6to4(tcp_in->check,
				pkt_ip6_hdr(in), &tcp_copy, sizeof(tcp_copy),
				pkt_ip4_hdr(out), tcp_out, sizeof(*tcp_out));
		out->skb->ip_summed = CHECKSUM_NONE;
	} else {
		tcp_out->check = update_csum_6to4_partial(tcp_in->check,
				pkt_ip6_hdr(in), pkt_ip4_hdr(out));
		partialize_skb(out->skb, offsetof(struct tcphdr, check));
	}

	return VERDICT_CONTINUE;
}

static verdict ttp64_udp(struct xlation *state, union flowix const *flowx)
{
	struct packet const *in = &state->in;
	struct packet *out = &state->out;
	struct udphdr const *udp_in = pkt_udp_hdr(in);
	struct udphdr *udp_out = pkt_udp_hdr(out);
	struct udphdr udp_copy;

	/* Header */
	memcpy(udp_out, udp_in, pkt_l4hdr_len(in));
	if (xlation_is_nat64(state)) {
		udp_out->source = get_src_port(out, flowx);
		udp_out->dest = get_dst_port(out, flowx);
	}

	/* Header.checksum */
	if (in->skb->ip_summed != CHECKSUM_PARTIAL) {
		memcpy(&udp_copy, udp_in, sizeof(*udp_in));
		udp_copy.check = 0;

		udp_out->check = 0;
		udp_out->check = update_csum_6to4(udp_in->check,
				pkt_ip6_hdr(in), &udp_copy, sizeof(udp_copy),
				pkt_ip4_hdr(out), udp_out, sizeof(*udp_out));
		if (udp_out->check == 0)
			udp_out->check = CSUM_MANGLED_0;
		out->skb->ip_summed = CHECKSUM_NONE;
	} else {
		udp_out->check = update_csum_6to4_partial(udp_in->check,
				pkt_ip6_hdr(in), pkt_ip4_hdr(out));
		partialize_skb(out->skb, offsetof(struct udphdr, check));
	}

	return VERDICT_CONTINUE;
}

const struct translation_steps ttp64_steps = {
	.skb_alloc = ttp64_alloc_skb,
	.xlat_outer_l3 = ttp64_ipv4_external,
	.xlat_inner_l3 = ttp64_ipv4_internal,
	.xlat_tcp = ttp64_tcp,
	.xlat_udp = ttp64_udp,
	.xlat_icmp = ttp64_icmp,
};
