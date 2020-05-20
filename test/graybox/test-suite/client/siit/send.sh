#!/bin/bash


# Arguments:
# $1: List of the names of the test groups you want to run, separated by any
#     character.
#     Example: "udp64, tcp46, icmpe64"
#     If this argument is unspecified, the script will run all the tests.
#     The current groups are:
#     - udp64: IPv6->IPv4 UDP tests (documented in ../../rfc/pktgen.md)
#     - udp46: IPv4->IPv6 UDP tests (documented in ../../rfc/pktgen.md)
#     - tcp64: IPv6->IPv4 TCP tests (documented in ../../rfc/pktgen.md)
#     - icmpi64: IPv6->IPv4 ICMP ping tests (documented in ../../rfc/pktgen.md)
#     - icmpi46: IPv4->IPv6 ICMP ping tests (documented in ../../rfc/pktgen.md)
#     - icmpe64: IPv6->IPv4 ICMP error tests (documented in ../../rfc/pktgen.md)
#     - icmpe46: IPv4->IPv6 ICMP error tests (documented in ../../rfc/pktgen.md)
#     - manual: random tests (documented in ../../rfc/manual.md)
#     - rfc7915: RFC 7915 compliance tests (documented in ../../rfc/7915.md)
#     (Feel free to add new groups if you want.)


GRAYBOX=`dirname $0`/../../../usr/graybox

# When Linux creates an ICMPv4 error on behalf of Jool, it writes 'c0' on the
# outer TOS field for me. This seems to mean "Network Control" messages
# according to DSCP, which is probably fair. Since TOS 0 would also be correct,
# we'll just accept whatever.
TOS=1
# The translated IPv4 identification is always random, so it should be always
# ignored during validation. Unfortunately, of course, the header checksum is
# also affected.
IDENTIFICATION=4,5,10,11
INNER_IDENTIFICATION=32,33,38,39


# Test boilerplate: pktgen tests.
function test-auto {
	$GRAYBOX expect add `dirname $0`/pktgen/receiver/$2.pkt $3
	$GRAYBOX send `dirname $0`/pktgen/sender/$1.pkt
	sleep 0.1
	$GRAYBOX expect flush
}

# Test boilerplate: Expects one packet, sends one.
function test11() {
	$GRAYBOX expect add `dirname $0`/$1/$2.pkt $4
	$GRAYBOX send `dirname $0`/$1/$3.pkt
	sleep 0.1
	$GRAYBOX expect flush
}

# Test boilerplate: Expects two packets, sends one.
function test21() {
	$GRAYBOX expect add `dirname $0`/$1/$2.pkt $5
	$GRAYBOX expect add `dirname $0`/$1/$3.pkt $5
	$GRAYBOX send `dirname $0`/$1/$4.pkt
	sleep 0.1
	$GRAYBOX expect flush
}


`dirname $0`/../wait.sh 2001:db8:1c6:3364:2::
if [ $? -ne 0 ]; then
	exit 1
fi

echo "Testing! Please wait..."


# UDP, 6 -> 4
if [[ -z $1 || $1 = *udp64* ]]; then
	test-auto 6-udp-csumok-df-nofrag 4-udp-csumok-df-nofrag $IDENTIFICATION
	test-auto 6-udp-csumok-nodf-nofrag 4-udp-csumok-nodf-nofrag
	test-auto 6-udp-csumok-nodf-frag0 4-udp-csumok-nodf-frag0
	test-auto 6-udp-csumok-nodf-frag1 4-udp-csumok-nodf-frag1
	test-auto 6-udp-csumok-nodf-frag2 4-udp-csumok-nodf-frag2

	test-auto 6-udp-csumfail-df-nofrag 4-udp-csumfail-df-nofrag $IDENTIFICATION
	test-auto 6-udp-csumfail-nodf-nofrag 4-udp-csumfail-nodf-nofrag
	test-auto 6-udp-csumfail-nodf-frag0 4-udp-csumfail-nodf-frag0
	test-auto 6-udp-csumfail-nodf-frag1 4-udp-csumfail-nodf-frag1
	test-auto 6-udp-csumfail-nodf-frag2 4-udp-csumfail-nodf-frag2
fi

# UDP, 4 -> 6
if [[ -z $1 || $1 = *udp46* ]]; then
	test-auto 4-udp-csumok-df-nofrag 6-udp-csumok-df-nofrag
	test-auto 4-udp-csumok-nodf-nofrag 6-udp-csumok-nodf-nofrag
	test-auto 4-udp-csumok-nodf-frag0 6-udp-csumok-nodf-frag0
	test-auto 4-udp-csumok-nodf-frag1 6-udp-csumok-nodf-frag1
	test-auto 4-udp-csumok-nodf-frag2 6-udp-csumok-nodf-frag2

	test-auto 4-udp-csumfail-df-nofrag 6-udp-csumfail-df-nofrag
	test-auto 4-udp-csumfail-nodf-nofrag 6-udp-csumfail-nodf-nofrag
	test-auto 4-udp-csumfail-nodf-frag0 6-udp-csumfail-nodf-frag0
	test-auto 4-udp-csumfail-nodf-frag1 6-udp-csumfail-nodf-frag1
	test-auto 4-udp-csumfail-nodf-frag2 6-udp-csumfail-nodf-frag2
fi

# TCP, 6 -> 4
if [[ -z $1 || $1 = *tcp64* ]]; then
	test-auto 6-tcp-csumok-df-nofrag 4-tcp-csumok-df-nofrag $IDENTIFICATION
	test-auto 6-tcp-csumok-nodf-nofrag 4-tcp-csumok-nodf-nofrag
	test-auto 6-tcp-csumok-nodf-frag0 4-tcp-csumok-nodf-frag0
	test-auto 6-tcp-csumok-nodf-frag1 4-tcp-csumok-nodf-frag1
	test-auto 6-tcp-csumok-nodf-frag2 4-tcp-csumok-nodf-frag2

	test-auto 6-tcp-csumfail-df-nofrag 4-tcp-csumfail-df-nofrag $IDENTIFICATION
	test-auto 6-tcp-csumfail-nodf-nofrag 4-tcp-csumfail-nodf-nofrag
	test-auto 6-tcp-csumfail-nodf-frag0 4-tcp-csumfail-nodf-frag0
	test-auto 6-tcp-csumfail-nodf-frag1 4-tcp-csumfail-nodf-frag1
	test-auto 6-tcp-csumfail-nodf-frag2 4-tcp-csumfail-nodf-frag2
fi

# ICMP info, 6 -> 4
if [[ -z $1 || $1 = *icmpi64* ]]; then
	test-auto 6-icmp6info-csumok-df-nofrag 4-icmp4info-csumok-df-nofrag $IDENTIFICATION
	test-auto 6-icmp6info-csumok-nodf-nofrag 4-icmp4info-csumok-nodf-nofrag

	test-auto 6-icmp6info-csumfail-df-nofrag 4-icmp4info-csumfail-df-nofrag $IDENTIFICATION
	test-auto 6-icmp6info-csumfail-nodf-nofrag 4-icmp4info-csumfail-nodf-nofrag
fi

# ICMP info, 4 -> 6
if [[ -z $1 || $1 = *icmpi46* ]]; then
	test-auto 4-icmp4info-csumok-df-nofrag 6-icmp6info-csumok-df-nofrag
	test-auto 4-icmp4info-csumok-nodf-nofrag 6-icmp6info-csumok-nodf-nofrag

	test-auto 4-icmp4info-csumfail-df-nofrag 6-icmp6info-csumfail-df-nofrag
	test-auto 4-icmp4info-csumfail-nodf-nofrag 6-icmp6info-csumfail-nodf-nofrag
fi

# ICMP error, 6 -> 4
if [[ -z $1 || $1 = *icmpe64* ]]; then
	# 22,23 = ICMP csum. Inherits the followind fields' randomness.
	# 32,33 = inner frag id. Same as above.
	# 34 = inner DF. An atomic fragments free Jool has no way to know the DF of the original packet.
	# 38,39 = inner IPv4 csum. Inherits other field's randomness.
	test-auto 6-icmp6err-csumok-df-nofrag 4-icmp4err-csumok-df-nofrag $IDENTIFICATION,22,23,32,33,34,38,39
	# This one doesn't have ignored bytes because DF and IDs have to be inferred from the fragment headers.
	test-auto 6-icmp6err-csumok-nodf-nofrag 4-icmp4err-csumok-nodf-nofrag
fi

# ICMP error, 4 -> 6
if [[ -z $1 || $1 = *icmpe46* ]]; then
	test-auto 4-icmp4err-csumok-df-nofrag 6-icmp6err-csumok-df-nofrag
	test-auto 4-icmp4err-csumok-nodf-nofrag 6-icmp6err-csumok-nodf-nofrag
fi

# "Manual" tests
if [[ -z $1 || $1 = *misc* ]]; then
	test11 manual 6791v64e 6791v64t $IDENTIFICATION,$INNER_IDENTIFICATION
	test11 manual 6791v66e 6791v66t
fi

# "RFC 6791" tests
if [[ -z $1 || $1 = *rfc7915* ]]; then
	# a
	test11 7915 aae1 aat1
	test11 7915 aae1 aat2
	test11 7915 aae1 aat3
	test11 7915 abe1 abt1 $IDENTIFICATION
	test11 7915 abe1 abt2 $IDENTIFICATION
	test11 7915 abe2 abt3
	test11 7915 ace1 act1
	test11 7915 ace2 act2
	test11 7915 ace3 act3
	test11 7915 ace4 act4
	test11 7915 ace5 act5
	test11 7915 ace6 act6
	test11 7915 ade1 adt1 $IDENTIFICATION
	test11 7915 ade2 adt2

	# b
	test11 7915 bae1 bat1
	test11 7915 bbe1 bbt1
	test11 7915 bce1 bct1
	test11 7915 bde1 bdt1 $IDENTIFICATION,$INNER_IDENTIFICATION
	test11 7915 bee1 bet1 $IDENTIFICATION,$INNER_IDENTIFICATION

	# c
	ip netns exec joolns jool_siit global update lowest-ipv6-mtu 1500
	ip netns exec joolns ip link set dev to_world_v6 mtu 1280
	test11 7915 cae1 cat1 $TOS,$IDENTIFICATION
	test11 7915 cae2 cat2 $TOS,$IDENTIFICATION
	test11 7915 cbe1 cbt1
	# Implementation quirk: The RFC wants us to copy the IPv4 identification
	# value (16 bits) to the IPv6 identification field (32 bits).
	# However, fragmentation is done by the kernel after the translation,
	# which means Jool does not get to decide the identification value.
	#
	# I think this is fine because identification preservation is only
	# relevant when the packet is already fragmented. As a matter of fact,
	# it's better if the kernel decides the identification because it will
	# generate a 32 bit number, and not be constrained to 16 bits like Jool.
	#
	# Identification preservation for already fragmented packets is tested
	# in cf.
	test21 7915 cce1 cce2 cct1 44,45,46,47
	test11 7915 cde1 cdt1
	test11 7915 cee1 cet1
	test11 7915 cee1 cet2
	test21 7915 cfe1 cfe2 cft1
	ip netns exec joolns ip link set dev to_world_v6 mtu 1500

	ip netns exec joolns ip link set dev to_world_v4 mtu 1400
	test11 7915 cge1 cgt1
	test11 7915 cge2 cgt2
	ip netns exec joolns ip link set dev to_world_v4 mtu 1500

	test11 7915 che1 cht1 $IDENTIFICATION
	test11 7915 cie1 cit1 $IDENTIFICATION
	test11 7915 cje1 cjt1
	test11 7915 cje2 cjt2
	test11 7915 cje3 cjt3
	test11 7915 cje4 cjt4

	ip netns exec joolns jool_siit global update lowest-ipv6-mtu 1280
	# This one is actually ck.
	test21 7915 cce1 cce2 cct1

	# d
	test11 7915 dae1 dat1 $IDENTIFICATION,$INNER_IDENTIFICATION
	test11 7915 dbe1 dbt1
	test11 7915 dbe2 dbt2
	test11 7915 dbe2 dbt3

	# e
	test11 7915 eae1 eat1 $TOS,$IDENTIFICATION
	ip netns exec joolns jool_siit global update amend-udp-checksum-zero 1
	test11 7915 ebe1 eat1
	test11 7915 ece1 ect1 $TOS,$IDENTIFICATION
	ip netns exec joolns jool_siit global update amend-udp-checksum-zero 0
	test11 7915 ece1 ect1 $TOS,$IDENTIFICATION

	# f
	test11 7915 fae1 fat1
	test11 7915 fbe1 fbt1 $IDENTIFICATION

	# g
	test11 7915 gae1 gat1

	# h
	test11 7915 hae1 hat1 $IDENTIFICATION,$INNER_IDENTIFICATION

	# i
	test11 7915 ic1e ic1t $IDENTIFICATION,$INNER_IDENTIFICATION
	test11 7915 ic2e ic2t $IDENTIFICATION,$INNER_IDENTIFICATION
	test11 7915 ic3e ic3t $IDENTIFICATION,$INNER_IDENTIFICATION
	test11 7915 ic4e ic4t $IDENTIFICATION,$INNER_IDENTIFICATION
	test11 7915 ic5e ic5t $IDENTIFICATION,$INNER_IDENTIFICATION
	test11 7915 ic6e ic6t $IDENTIFICATION,$INNER_IDENTIFICATION
	test11 7915 idae idat
	test11 7915 idbe idbt
	test11 7915 idce idct
	test11 7915 idde iddt
	test11 7915 idee idet
	test11 7915 idfe idft
	test11 7915 idhe idht
	test11 7915 idze idzt

	ip link set dev to_jool_v4 mtu 2153
	ip netns exec joolns ip link set dev to_world_v4 mtu 2153
	sleep 0.5
	test11 7915 idge idgt
	test11 7915 idie idit
	test11 7915 idye idyt
	ip link set dev to_jool_v4 mtu 1500
	ip netns exec joolns ip link set dev to_world_v4 mtu 1500

	# j
	ip netns exec joolns jool_siit global update lowest-ipv6-mtu 1500
	test11 7915 jae jat
	ip netns exec joolns jool_siit global update lowest-ipv6-mtu 1280

	test21 7915 jbae1 jbae2 jbat
	test11 7915 jbbe jbbt
	ip netns exec joolns jool_siit global update force-slow-path-46 true
	test21 7915 jbae1 jbae2 jbat
	test21 7915 jbae1 jbae2 jbbt
	ip netns exec joolns jool_siit global update force-slow-path-46 false

	ip netns exec joolns jool_siit global update lowest-ipv6-mtu 1402
	test21 7915 jcae1 jcae2 jcat
	test11 7915 jcbe jcbt
	ip netns exec joolns jool_siit global update force-slow-path-46 true
	test21 7915 jcae1 jcae2 jcat
	test21 7915 jcae1 jcae2 jcbt
	ip netns exec joolns jool_siit global update force-slow-path-46 false
	ip netns exec joolns jool_siit global update lowest-ipv6-mtu 1280
fi

#if [[ -z $1 || $1 = *new* ]]; then
#fi

$GRAYBOX stats display
result=$?
$GRAYBOX stats flush

exit $result
