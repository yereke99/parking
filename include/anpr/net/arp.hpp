#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "anpr/net/socket.hpp"

namespace anpr::net {

struct ArpEntry {
    Ipv4 ip;
    /// Normalized MAC, empty for an incomplete entry.
    std::string mac;
    std::string interface;
    /// ATF_COM (0x2): the neighbour answered.
    bool complete{false};
};

/// Parses /proc/net/arp. Incomplete entries are kept with `complete` false.
std::vector<ArpEntry> parseProcNetArp(const std::string& text);
std::vector<ArpEntry> readArpTable(const std::string& path = "/proc/net/arp");

struct ArpProbeResult {
    /// False when raw sockets are not available (no CAP_NET_RAW, not Linux). Not an error for the
    /// caller: duplicate detection then relies on SADP and the neighbour table only.
    bool supported{false};
    std::string error;
    /// Every MAC that answered for each probed address (host-order address value -> MACs).
    std::map<std::uint32_t, std::set<std::string>> replies;
};

/// Sends one ARP request per target on `interface` (sender `sender_ip`, or 0.0.0.0 for an RFC 5227
/// style probe) and collects every reply for `listen_ms`. Two different MACs answering for one
/// address is a duplicate IP. Uses an AF_PACKET socket: needs CAP_NET_RAW, which Docker grants by
/// default.
ArpProbeResult arpProbe(const std::string& interface, Ipv4 sender_ip,
                        const std::vector<Ipv4>& targets, int listen_ms);

}  // namespace anpr::net
