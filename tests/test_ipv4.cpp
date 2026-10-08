#include "anpr/net/socket.hpp"

#include "test_framework.hpp"

using anpr::net::Ipv4;
using anpr::net::Ipv4Network;

TEST("ipv4 parses dotted quads and rejects junk") {
    const auto parsed = anpr::net::parseIpv4("192.168.10.21");
    CHECK(parsed.has_value());
    CHECK_EQ(anpr::net::toString(*parsed), std::string("192.168.10.21"));
    CHECK(!anpr::net::parseIpv4("192.168.10").has_value());
    CHECK(!anpr::net::parseIpv4("192.168.10.256").has_value());
    CHECK(!anpr::net::parseIpv4("192.168.010.1").has_value());
    CHECK(!anpr::net::parseIpv4("192.168.10.1 ").has_value());
    CHECK(!anpr::net::parseIpv4("").has_value());
    CHECK(anpr::net::parseIpv4("0.0.0.0").has_value());
}

TEST("host:port parsing keeps the default port when none is given") {
    Ipv4 host;
    std::uint16_t port = 554;
    CHECK(anpr::net::parseHostPort("192.168.10.22", host, port));
    CHECK_EQ(port, std::uint16_t{554});
    CHECK(anpr::net::parseHostPort("192.168.10.22:8554", host, port));
    CHECK_EQ(port, std::uint16_t{8554});
    CHECK_EQ(anpr::net::toString(host), std::string("192.168.10.22"));
    CHECK(!anpr::net::parseHostPort("192.168.10.22:0", host, port));
    CHECK(!anpr::net::parseHostPort("camera:554", host, port));
}

TEST("ipv4 networks compute masks, ranges and overlaps") {
    const Ipv4Network lan{*anpr::net::parseIpv4("192.168.10.5"), 24};
    CHECK_EQ(lan.cidr(), std::string("192.168.10.0/24"));
    CHECK_EQ(lan.addressWithPrefix(), std::string("192.168.10.5/24"));
    CHECK_EQ(anpr::net::toString(lan.broadcast()), std::string("192.168.10.255"));
    CHECK(lan.contains(*anpr::net::parseIpv4("192.168.10.254")));
    CHECK(!lan.contains(*anpr::net::parseIpv4("192.168.1.64")));
    CHECK_EQ(lan.hostCount(), std::uint32_t{254});
    const auto hosts = lan.hosts(1024);
    CHECK_EQ(hosts.size(), std::size_t{254});
    CHECK_EQ(anpr::net::toString(hosts.front()), std::string("192.168.10.1"));
    CHECK_EQ(anpr::net::toString(hosts.back()), std::string("192.168.10.254"));
    CHECK(lan.hosts(100).empty());
    const Ipv4Network docker{*anpr::net::parseIpv4("192.168.0.1"), 16};
    CHECK(lan.overlaps(docker));
    CHECK(!lan.overlaps(Ipv4Network{*anpr::net::parseIpv4("172.17.0.1"), 16}));
    CHECK_EQ(anpr::net::prefixFromNetmask(*anpr::net::parseIpv4("255.255.255.0")), 24);
    CHECK_EQ(anpr::net::prefixFromNetmask(*anpr::net::parseIpv4("255.255.0.255")), -1);
    CHECK(anpr::net::isLinkLocal(*anpr::net::parseIpv4("169.254.3.4")));
    CHECK(anpr::net::isMulticast(*anpr::net::parseIpv4("239.255.255.250")));
}

TEST("mac addresses normalize from every common notation") {
    CHECK_EQ(anpr::net::normalizeMac("44-19-B6-01-02-03"), std::string("44:19:b6:01:02:03"));
    CHECK_EQ(anpr::net::normalizeMac("4419.b601.0203"), std::string("44:19:b6:01:02:03"));
    CHECK_EQ(anpr::net::normalizeMac("4419b6010203"), std::string("44:19:b6:01:02:03"));
    CHECK_EQ(anpr::net::normalizeMac("00:00:00:00:00:00"), std::string());
    CHECK_EQ(anpr::net::normalizeMac("44:19:b6:01:02"), std::string());
    CHECK_EQ(anpr::net::normalizeMac("zz:19:b6:01:02:03"), std::string());
    CHECK(anpr::net::isHikvisionOui("44:19:b6:01:02:03"));
    CHECK(!anpr::net::isHikvisionOui("00:11:22:33:44:55"));
}
