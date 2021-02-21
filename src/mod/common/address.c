#include "mod/common/address.h"

#include <linux/inet.h>

bool taddr6_equals(const struct ipv6_transport_addr *a,
		const struct ipv6_transport_addr *b)
{
	return addr6_equals(&a->l3, &b->l3) && (a->l4 == b->l4);
}
EXPORT_UNIT_SYMBOL(taddr6_equals)

bool taddr4_equals(const struct ipv4_transport_addr *a,
		const struct ipv4_transport_addr *b)
{
	return addr4_equals(&a->l3, &b->l3) && (a->l4 == b->l4);
}
EXPORT_UNIT_SYMBOL(taddr4_equals)

bool prefix6_equals(const struct ipv6_prefix *a, const struct ipv6_prefix *b)
{
	return addr6_equals(&a->addr, &b->addr) && (a->len == b->len);
}

bool prefix4_equals(const struct ipv4_prefix *a, const struct ipv4_prefix *b)
{
	return addr4_equals(&a->addr, &b->addr) && (a->len == b->len);
}

__u32 get_prefix4_mask(const struct ipv4_prefix *prefix)
{
	return ((__u64) 0xffffffffU) << (32 - prefix->len);
}

bool __prefix4_contains(const struct ipv4_prefix *prefix, __be32 addr)
{
	__u32 maskbits = get_prefix4_mask(prefix);
	__u32 prefixbits = be32_to_cpu(prefix->addr.s_addr) & maskbits;
	__u32 addrbits = be32_to_cpu(addr) & maskbits;
	return prefixbits == addrbits;
}

bool prefix4_contains(const struct ipv4_prefix *prefix,
		struct in_addr const *addr)
{
	return __prefix4_contains(prefix, addr->s_addr);
}
EXPORT_UNIT_SYMBOL(prefix4_contains)

bool prefix4_intersects(const struct ipv4_prefix *p1,
		const struct ipv4_prefix *p2)
{
	return prefix4_contains(p1, &p2->addr)
			|| prefix4_contains(p2, &p1->addr);
}

__u64 prefix4_get_addr_count(const struct ipv4_prefix *prefix)
{
	return ((__u64) 1U) << (32 - prefix->len);
}
EXPORT_UNIT_SYMBOL(prefix4_get_addr_count)

bool prefix6_contains(const struct ipv6_prefix *prefix,
		const struct in6_addr *addr)
{
	return ipv6_prefix_equal(&prefix->addr, addr, prefix->len);
}
EXPORT_UNIT_SYMBOL(prefix6_contains)

__u32 addr4_get_bit(const struct in_addr *addr, unsigned int pos)
{
	__u32 mask = 1U << (31 - pos);
	return be32_to_cpu(addr->s_addr) & mask;
}

void addr4_set_bit(struct in_addr *addr, unsigned int pos, bool value)
{
	__u32 mask = 1U << (31 - pos);

	if (value)
		addr->s_addr |= cpu_to_be32(mask);
	else
		addr->s_addr &= cpu_to_be32(~mask);
}

__u32 addr6_get_bit(const struct in6_addr *addr, unsigned int pos)
{
	__u32 quadrant; /* As in, @addr has 4 "quadrants" of 32 bits each. */
	__u32 mask;

	/* "pos >> 5" is a more efficient version of "pos / 32". */
	quadrant = be32_to_cpu(addr->s6_addr32[pos >> 5]);
	/* "pos & 0x1FU" is a more efficient version of "pos % 32". */
	mask = 1U << (31 - (pos & 0x1FU));

	return quadrant & mask;
}

void addr6_set_bit(struct in6_addr *addr, unsigned int pos, bool value)
{
	__be32 *quadrant;
	__u32 mask;

	quadrant = &addr->s6_addr32[pos >> 5];
	mask = 1U << (31 - (pos & 0x1FU));

	if (value)
		*quadrant |= cpu_to_be32(mask);
	else
		*quadrant &= cpu_to_be32(~mask);
}

unsigned int addr4_get_bits(__be32 addr, unsigned int offset,
		unsigned int len)
{
	unsigned int result;
	result = be32_to_cpu(addr) >> (32u - offset - len);
	return (len != 32) ? (result & ((1u << len) - 1u)) : result;
}

unsigned int addr6_get_bits(struct in6_addr const *addr,
		unsigned int offset, unsigned int len)
{
	unsigned int i;
	unsigned int result;

	result = 0;
	for (i = 0; i < len; i++)
		if (addr6_get_bit(addr, i + offset))
			result |= 1 << (len - i - 1);

	return result;
}

/* TODO (performance) if bits are aligned, copy bytes instead. */
void addr6_set_bits(struct in6_addr *addr, unsigned int offset,
		unsigned int len, unsigned int value)
{
	unsigned int i;
	for (i = 0; i < len; i++)
		addr6_set_bit(addr, offset + i, (value >> (len - i - 1u)) & 1u);
}

/**
 * Like addr6_copy_bits(), except it assumes all the bits to be copied are
 * located in the same byte.
 */
static void __addr6_copy_bits(struct in6_addr *asrc, struct in6_addr *adst,
		unsigned int offset, unsigned int len)
{
	__u8 src, *dst;
	__u8 mask;

	src = asrc->s6_addr[offset >> 3];
	dst = &adst->s6_addr[offset >> 3];
	offset &= 7u;
	mask = ((1u << len) - 1u) << (8u - offset - len);

	*dst = ((*dst) & ~mask) | (src & mask);
}

void addr6_copy_bits(struct in6_addr *src, struct in6_addr *dst,
		unsigned int offset, unsigned int len)
{
	unsigned int delta;

	/* Rightmost bits of left byte */
	if (offset & 7u) {
		if (offset + len > (offset | 7u)) {
			delta = (offset | 7u) - offset + 1;
			__addr6_copy_bits(src, dst, offset, delta);
			offset += delta;
			len -= delta;
		} else {
			__addr6_copy_bits(src, dst, offset, len);
			return;
		}
	}

	/* Middle bytes */
	if (offset + len > (offset | 7u)) {
		memcpy(&dst->s6_addr[offset >> 3u], &src->s6_addr[offset >> 3u],
				len >> 3);
		delta = len & ~7u;
		offset += delta;
		len -= delta;
	}

	/* Leftmost bits of right byte */
	if (len > 0)
		__addr6_copy_bits(src, dst, offset, len);
}
EXPORT_UNIT_SYMBOL(addr6_copy_bits)

__u64 prefix4_next(const struct ipv4_prefix *prefix)
{
	return prefix4_get_addr_count(prefix)
			+ (__u64) be32_to_cpu(prefix->addr.s_addr);
}

/**
 * addr4_has_scope_subnet - returns true if @addr has low scope ("this" subnet
 * or lower), and therefore should not be translated under any circumstances.
 */
bool addr4_is_scope_subnet(const __be32 addr)
{
	/*
	 * I'm assuming private and doc networks do not belong to this category,
	 * to facilitate testing.
	 * (particularly users following the tutorials verbatim.)
	 */
	return ipv4_is_zeronet(addr)
			|| ipv4_is_loopback(addr)
			|| ipv4_is_linklocal_169(addr)
			|| ipv4_is_multicast(addr)
			|| ipv4_is_lbcast(addr);
}

/**
 * prefix4_has_subnet_scope - returns true if @prefix intersects with one of the
 * low-scoped networks ("this" subnet or lower), false otherwise.
 * If @subnet is sent, the colliding subnet is copied to it.
 */
bool prefix4_has_subnet_scope(struct ipv4_prefix *prefix,
		struct ipv4_prefix *subnet)
{
	struct ipv4_prefix subnets[] = {
			{ .addr.s_addr = cpu_to_be32(0x00000000), .len = 8 },
			{ .addr.s_addr = cpu_to_be32(0x7f000000), .len = 8 },
			{ .addr.s_addr = cpu_to_be32(0xa9fe0000), .len = 16 },
			{ .addr.s_addr = cpu_to_be32(0xe0000000), .len = 4 },
			{ .addr.s_addr = cpu_to_be32(0xffffffff), .len = 32 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(subnets); i++) {
		if (prefix4_intersects(prefix, &subnets[i])) {
			if (subnet)
				*subnet = subnets[i];
			return true;
		}
	}

	return false;
}

int taddr6_compare(const struct ipv6_transport_addr *a1,
		const struct ipv6_transport_addr *a2)
{
	int gap;

	gap = ipv6_addr_cmp(&a1->l3, &a2->l3);
	if (gap)
		return gap;

	return ((int)a1->l4) - ((int)a2->l4);
}

int taddr4_compare(const struct ipv4_transport_addr *a1,
		const struct ipv4_transport_addr *a2)
{
	int gap;

	gap = ipv4_addr_cmp(&a1->l3, &a2->l3);
	if (gap)
		return gap;

	return ((int)a1->l4) - ((int)a2->l4);
}

bool maprule_equals(struct mapping_rule *r1, struct mapping_rule *r2)
{
	return prefix6_equals(&r1->prefix6, &r2->prefix6)
	    && prefix4_equals(&r1->prefix4, &r2->prefix4)
	    && r1->o == r2->o
	    && r1->a == r2->a;
}
