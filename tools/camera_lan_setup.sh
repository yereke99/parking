#!/usr/bin/env bash
# Gives the Jetson a permanent static address on the camera LAN, with NetworkManager.
#
#   tools/camera_lan_setup.sh [--interface eth0] --address 192.168.10.5/24 [--apply] [--yes]
#   tools/camera_lan_setup.sh --show
#   tools/camera_lan_setup.sh --remove [--yes]
#   make camera-lan-setup ADDRESS=192.168.10.5/24                      (dry run)
#   make camera-lan-setup ADDRESS=192.168.10.5/24 LAN_ARGS=--apply
#
# The PoE switch has no DHCP server, so the Jetson needs a static address in the cameras' subnet.
# `ip addr add` does not last: NetworkManager flushes the addresses of a device it manages at its
# next failed DHCP attempt or cable replug. This adds the NetworkManager profile kz-camera-lan
# instead: a manual address, never the default route, no gateway, no DNS, and the highest
# autoconnect priority on that interface. The GSM modem's connection and the routes are left
# alone; if the default route changes anyway, the profile is removed again.
#
# Without --apply nothing changes: it prints the interfaces, the NetworkManager connections, the
# default routes and the exact nmcli commands. Repeat --address (or separate addresses with
# commas) to also reach a factory-default camera at 192.168.1.64 while re-addressing it:
# --address 192.168.10.5/24 --address 192.168.1.10/24. Run it on the Jetson, not in Docker.
#
# KZ_SYS_CLASS_NET, KZ_PROC_NET_ROUTE, KZ_PROC_IPV4_CONF and KZ_LAN_SETTLE_SECONDS exist for the
# tests.
set -euo pipefail

connection=kz-camera-lan
sys_net="${KZ_SYS_CLASS_NET:-/sys/class/net}"
proc_route="${KZ_PROC_NET_ROUTE:-/proc/net/route}"
ipv4_conf="${KZ_PROC_IPV4_CONF:-/proc/sys/net/ipv4/conf}"
settle_seconds="${KZ_LAN_SETTLE_SECONDS:-3}"

usage() {
    cat >&2 <<'EOF'
usage: tools/camera_lan_setup.sh [--interface IF] --address IP/PREFIX [--apply] [--yes]
       tools/camera_lan_setup.sh [--interface IF] --show
       tools/camera_lan_setup.sh --remove [--yes]

  --address IP/PREFIX  the Jetson's address in the cameras' subnet, e.g. 192.168.10.5/24;
                       repeat it (or separate with commas) for a second subnet
  --interface IF       the Ethernet port wired to the PoE switch (default: detected)
  --apply              make the change (default: dry run, prints the nmcli commands)
  --show               only print interfaces, NetworkManager connections and default routes
  --remove             delete the kz-camera-lan profile
  --yes                do not ask for confirmation
EOF
}

die() {
    local status="$1"
    shift
    echo "ERROR: $*" >&2
    exit "$status"
}

interface=""
addresses=()
apply=0
show=0
remove=0
assume_yes=0
while (( $# > 0 )); do
    case "$1" in
        --interface|--address)
            (( $# >= 2 )) || { usage; exit 2; }
            option="$1"
            value="$2"
            shift 2
            ;;
        --interface=*|--address=*)
            option="${1%%=*}"
            value="${1#*=}"
            shift
            ;;
        --apply) apply=1; shift; continue ;;
        --show) show=1; shift; continue ;;
        --remove) remove=1; shift; continue ;;
        --yes|-y) assume_yes=1; shift; continue ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
    if [[ "$option" == --interface ]]; then
        interface="$value"
    else
        IFS=',' read -r -a parts <<< "$value"
        for part in "${parts[@]}"; do
            part="${part//[[:space:]]/}"
            if [[ -n "$part" ]]; then
                addresses+=("$part")
            fi
        done
    fi
done
if (( show + remove + apply > 1 )); then
    die 2 "--show, --remove and --apply exclude each other."
fi
if (( show || remove )) && (( ${#addresses[@]} > 0 )); then
    die 2 "--address goes with a dry run or --apply, not with --show or --remove."
fi
if (( apply )) && (( ${#addresses[@]} == 0 )); then
    die 2 "--apply needs --address, for example --address 192.168.10.5/24."
fi
if [[ -n "$interface" && ! "$interface" =~ ^[A-Za-z0-9_.-]{1,15}$ ]]; then
    die 2 "invalid interface name: $interface"
fi
if [[ ! -d "$sys_net" || ! -r "$proc_route" ]]; then
    die 2 "$sys_net or $proc_route is missing: run this on the Jetson (Linux) itself."
fi

sudo_word=""
if [[ "$(id -u)" != 0 ]]; then
    sudo_word="sudo "
fi

# Runs a NetworkManager change as root.
privileged() {
    if [[ -n "$sudo_word" ]]; then
        sudo "$@"
    else
        "$@"
    fi
}

# ---- Reading the host: sysfs, /proc/net/route, ip, nmcli ---------------------------------------

read_sys() {
    local value=""
    { IFS= read -r value < "$sys_net/$1/$2"; } 2>/dev/null || true
    printf '%s' "$value"
}

uevent_value() {
    local line file="$sys_net/$1/uevent"
    [[ -r "$file" ]] || return 0
    while IFS= read -r line; do
        if [[ "$line" == "$2="* ]]; then
            printf '%s' "${line#*=}"
            return 0
        fi
    done < "$file"
}

driver_of() {
    local link
    link="$(readlink "$sys_net/$1/device/driver" 2>/dev/null || true)"
    printf '%s' "${link##*/}"
}

# USB makers of cellular modems (the C++ list): a modem presenting cdc_ether / cdc_ncm looks like
# a USB Ethernet adapter otherwise.
is_modem_vendor() {
    case "$1" in
        12d1|19d2|1bbb|2c7c|1e0e|1199|2cb7|1508|05c6|1bc7|1546|0bdb|413c|1410|0af0|1e2d) return 0 ;;
    esac
    return 1
}

# Decided from sysfs only, never from the name: a GSM modem may be eth1, usb0 is the Jetson's
# own USB gadget. The C++ network topology's rules, a little stricter: a phone tethered over
# rndis_host or ipheth never counts as the camera LAN either.
interface_kind() {
    local name="$1" base="$sys_net/$1" type devtype driver vendor
    type="$(read_sys "$name" type)"
    devtype="$(uevent_value "$name" DEVTYPE)"
    if [[ "$type" == 772 ]]; then echo loopback; return; fi
    if [[ "$devtype" == gadget ]]; then echo usb_gadget; return; fi
    if [[ "$devtype" == bridge || -d "$base/bridge" ]]; then echo bridge; return; fi
    if [[ -d "$base/brport" ]]; then echo bridge_port; return; fi
    if [[ "$type" == 512 || "$devtype" == wwan ]]; then echo cellular; return; fi
    if [[ ! -e "$base/device" ]]; then echo virtual; return; fi
    if [[ "$devtype" == wlan || -e "$base/phy80211" || -d "$base/wireless" ]]; then
        echo wireless
        return
    fi
    driver="$(driver_of "$name")"
    vendor="$(read_sys "$name" device/../idVendor)"
    case "$driver" in
        qmi_wwan|cdc_mbim|huawei_cdc_ncm|rndis_host|ipheth)
            echo cellular
            return
            ;;
        cdc_ether|cdc_ncm|option|sierra_net)
            if is_modem_vendor "$vendor"; then echo cellular; return; fi
            ;;
    esac
    if [[ "$type" == 1 ]]; then echo ethernet; return; fi
    echo other
}

# "up", "down" or "-" (administratively down: the kernel cannot tell).
link_of() {
    case "$(read_sys "$1" carrier)" in
        1) echo up ;;
        0) echo down ;;
        *) echo - ;;
    esac
}

hex_to_ip() {
    local hex="$1"
    # /proc/net/route prints the raw big-endian word on a little-endian CPU: bytes reversed.
    printf '%d.%d.%d.%d' "$((16#${hex:6:2}))" "$((16#${hex:4:2}))" "$((16#${hex:2:2}))" \
        "$((16#${hex:0:2}))"
}

# "IFACE GATEWAY METRIC" per default route that is up, sorted.
default_routes() {
    local iface destination gateway flags refcnt use metric mask rest
    {
        read -r _ || true
        while read -r iface destination gateway flags refcnt use metric mask rest; do
            if [[ "$destination" == 00000000 && "$mask" == 00000000 &&
                  "$flags" =~ ^[0-9A-Fa-f]+$ ]] && (( (16#$flags & 1) != 0 )); then
                printf '%s %s %s\n' "$iface" "$(hex_to_ip "$gateway")" "$metric"
            fi
        done
    } < "$proc_route" | sort
}

# The default routes on one line: "wwan0 10.64.64.1 700; eth1 192.168.8.1 750" or "none".
route_summary() {
    local joined
    joined="$(paste -sd';' - <<< "$1" | sed 's/;/; /g')"
    echo "${joined:-none}"
}

carries_default_route() {
    local iface gateway metric
    while read -r iface gateway metric; do
        if [[ "$iface" == "$1" ]]; then
            return 0
        fi
    done <<< "$(default_routes)"
    return 1
}

ip_addresses=""
if command -v ip >/dev/null 2>&1; then
    ip_addresses="$(ip -4 -o addr show 2>/dev/null || true)"
fi

# The IPv4 addresses of interface $1 as "a.b.c.d/p" lines.
addresses_of() {
    local index name family address rest
    while read -r index name family address rest; do
        name="${name%%@*}"
        if [[ "$name" == "$1" && "$family" == inet ]]; then
            echo "$address"
        fi
    done <<< "$ip_addresses"
}

have_nmcli=0
nm_running=0
if command -v nmcli >/dev/null 2>&1; then
    have_nmcli=1
    if [[ "$(nmcli -t -f RUNNING general 2>/dev/null || true)" == running ]]; then
        nm_running=1
    fi
fi

nm_device_state() {
    local line
    while IFS= read -r line; do
        if [[ "${line%%:*}" == "$1" ]]; then
            printf '%s' "${line#*:}"
            return 0
        fi
    done <<< "$(nmcli -t -f DEVICE,STATE device 2>/dev/null || true)"
}

connection_exists() {
    local line
    while IFS= read -r line; do
        if [[ "$line" == "$connection" ]]; then
            return 0
        fi
    done <<< "$(nmcli -t -f NAME connection show 2>/dev/null || true)"
    return 1
}

# Prints the value of one setting of the kz-camera-lan profile.
connection_setting() {
    local line
    while IFS= read -r line; do
        if [[ "${line%%:*}" == "$1" ]]; then
            printf '%s' "${line#*:}"
            return 0
        fi
    done <<< "$(nmcli -t -f "$1" connection show "$connection" 2>/dev/null || true)"
}

# ---- Addresses ---------------------------------------------------------------------------------

octet='(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])'
ipv4_pattern="^$octet\\.$octet\\.$octet\\.$octet\$"

ip_to_int() {
    local a b c d
    IFS=. read -r a b c d <<< "$1"
    echo $(( (a << 24) | (b << 16) | (c << 8) | d ))
}

int_to_ip() {
    echo "$(( ($1 >> 24) & 255 )).$(( ($1 >> 16) & 255 )).$(( ($1 >> 8) & 255 )).$(( $1 & 255 ))"
}

prefix_mask() {
    echo $(( (0xFFFFFFFF << (32 - $1)) & 0xFFFFFFFF ))
}

# "a.b.c.d/p" -> "network/p".
network_of() {
    local ip="${1%/*}" prefix="${1#*/}" mask
    mask="$(prefix_mask "$prefix")"
    echo "$(int_to_ip $(( $(ip_to_int "$ip") & mask )))/$prefix"
}

# True when the two "a.b.c.d/p" subnets share any address.
subnets_overlap() {
    local first="$1" second="$2" prefix mask
    prefix="${first#*/}"
    if (( ${second#*/} < prefix )); then
        prefix="${second#*/}"
    fi
    mask="$(prefix_mask "$prefix")"
    (( ($(ip_to_int "${first%/*}") & mask) == ($(ip_to_int "${second%/*}") & mask) ))
}

validate_address() {
    local address="$1" ip prefix value mask host_bits
    if [[ "$address" != */* ]]; then
        die 2 "$address has no prefix length. Give the camera subnet's, for example $address/24" \
            "(without it NetworkManager assumes /32 and no camera is reachable)."
    fi
    ip="${address%/*}"
    prefix="${address#*/}"
    if [[ ! "$ip" =~ $ipv4_pattern ]]; then
        die 2 "$address is not an IPv4 address."
    fi
    if [[ ! "$prefix" =~ ^[0-9]+$ ]] || (( 10#$prefix < 8 || 10#$prefix > 30 )); then
        die 2 "$address: the prefix length must be between 8 and 30 (24 for 255.255.255.0)."
    fi
    value="$(ip_to_int "$ip")"
    case "${ip%%.*}" in
        0|127) die 2 "$address is not usable on a LAN." ;;
    esac
    if (( (value >> 16) == 0xA9FE )); then
        die 2 "$address is link-local (169.254.0.0/16); cameras need a static address."
    fi
    if (( (value >> 28) >= 14 )); then
        die 2 "$address is a multicast or reserved address."
    fi
    mask="$(prefix_mask "$((10#$prefix))")"
    host_bits=$(( value & ~mask & 0xFFFFFFFF ))
    if (( host_bits == 0 || host_bits == (~mask & 0xFFFFFFFF) )); then
        die 2 "$address is the network or broadcast address of its subnet; pick a host address."
    fi
    if subnets_overlap "$ip/$((10#$prefix))" 172.16.0.0/12; then
        echo "WARNING: $address lies in 172.16.0.0/12, where Docker creates its networks." \
            "A Docker network in the same range would take the camera traffic." >&2
    fi
}

# ---- Report ------------------------------------------------------------------------------------

all_interfaces() {
    local path
    for path in "$sys_net"/*; do
        if [[ -e "$path" ]]; then
            echo "${path##*/}"
        fi
    done | sort
}

print_state() {
    local name kind addrs driver marker iface gateway metric all value effective routes
    echo "== Network interfaces"
    printf '  %-12s %-12s %-5s %-34s %s\n' NAME KIND LINK IPV4 DRIVER
    while read -r name; do
        kind="$(interface_kind "$name")"
        if [[ "$kind" == loopback ]]; then
            continue
        fi
        addrs="$(addresses_of "$name" | paste -sd, -)"
        driver="$(driver_of "$name")"
        marker=""
        if carries_default_route "$name"; then
            marker="  (default route)"
        fi
        printf '  %-12s %-12s %-5s %-34s %s%s\n' "$name" "$kind" "$(link_of "$name")" \
            "${addrs:--}" "${driver:--}" "$marker"
    done <<< "$(all_interfaces)"
    if [[ -z "$ip_addresses" ]]; then
        echo "  (the ip command is missing, so addresses are not shown)"
    fi

    echo "== Default routes (Internet)"
    routes="$(default_routes)"
    if [[ -z "$routes" ]]; then
        echo "  none: no Internet uplink is configured (camera processing does not need one)"
    else
        while read -r iface gateway metric; do
            echo "  $iface via $gateway metric $metric"
        done <<< "$routes"
    fi

    echo "== NetworkManager"
    if (( ! have_nmcli )); then
        echo "  nmcli is not installed"
    elif (( ! nm_running )); then
        echo "  NetworkManager is not running"
    else
        echo "  devices (DEVICE:TYPE:STATE:CONNECTION):"
        nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device 2>/dev/null | sed 's/^/    /' || true
        echo "  connections (NAME:TYPE:DEVICE):"
        nmcli -t -f NAME,TYPE,DEVICE connection show 2>/dev/null | sed 's/^/    /' || true
        if connection_exists; then
            echo "  $connection: $(connection_setting ipv4.addresses) on" \
                "$(connection_setting connection.interface-name)"
        fi
    fi

    if [[ -n "${1:-}" ]]; then
        all="$(read_sys_abs "$ipv4_conf/all/rp_filter")"
        value="$(read_sys_abs "$ipv4_conf/$1/rp_filter")"
        effective="$all"
        if [[ "$value" =~ ^[0-9]$ && ( ! "$all" =~ ^[0-9]$ || "$value" -gt "$all" ) ]]; then
            effective="$value"
        fi
        echo "== Reverse-path filter: all=${all:--} $1=${value:--}"
        if [[ "$effective" == 1 ]]; then
            echo "  strict: replies from a camera outside $1's subnets are dropped, so a camera on"
            echo "  another subnet stays invisible to discovery (docs/CAMERAS.md, rp_filter)."
        fi
    fi
}

read_sys_abs() {
    local value=""
    { IFS= read -r value < "$1"; } 2>/dev/null || true
    printf '%s' "$value"
}

# ---- The camera LAN interface ------------------------------------------------------------------

choose_interface() {
    local name kind candidates=() wired=()
    while read -r name; do
        kind="$(interface_kind "$name")"
        if [[ "$kind" != ethernet ]]; then
            continue
        fi
        wired+=("$name ($(link_of "$name"))")
        if [[ "$(link_of "$name")" == up ]] && ! carries_default_route "$name"; then
            candidates+=("$name")
        fi
    done <<< "$(all_interfaces)"
    if (( ${#candidates[@]} == 1 )); then
        echo "${candidates[0]}"
        return 0
    fi
    if (( ${#candidates[@]} == 0 )); then
        echo "ERROR: no wired Ethernet interface with link that is free of the default route." >&2
        if (( ${#wired[@]} > 0 )); then
            echo "Wired interfaces: ${wired[*]}." >&2
        fi
        echo "Check the cable from the Jetson to the PoE switch and the switch power, or pass" \
            "--interface." >&2
    else
        echo "ERROR: several wired interfaces have link: ${candidates[*]}. Pass --interface" \
            "(INTERFACE= with make)." >&2
    fi
    return 1
}

check_interface() {
    local kind
    if [[ ! -e "$sys_net/$1" ]]; then
        die 2 "interface $1 does not exist."
    fi
    kind="$(interface_kind "$1")"
    if [[ "$kind" != ethernet ]]; then
        die 2 "$1 is a $kind interface, not a wired Ethernet port. The camera LAN is the" \
            "Ethernet port wired to the PoE switch; the modem's connection is never changed."
    fi
    if carries_default_route "$1"; then
        die 2 "$1 carries the default route, so Internet traffic uses it. Replacing its" \
            "NetworkManager connection would cut that off; refusing."
    fi
}

# ---- Main --------------------------------------------------------------------------------------

if (( show )); then
    if [[ -n "$interface" ]]; then
        [[ -e "$sys_net/$interface" ]] || die 2 "interface $interface does not exist."
    else
        interface="$(choose_interface 2>/dev/null || true)"
    fi
    print_state "$interface"
    echo "== Camera LAN interface: ${interface:-not found}"
    exit 0
fi

if (( remove )); then
    (( have_nmcli && nm_running )) || die 3 "NetworkManager (nmcli) is not available."
    if ! connection_exists; then
        echo "There is no $connection profile; nothing to remove."
        exit 0
    fi
    echo "$connection: $(connection_setting ipv4.addresses) on" \
        "$(connection_setting connection.interface-name)"
    echo "Removing it leaves the Jetson without an address on the camera LAN: no camera is" \
        "reachable until it has another one."
    printf 'Delete %s? [y/N] ' "$connection" >&2
    answer=""
    if (( assume_yes )); then
        answer=y
        echo y >&2
    elif ! IFS= read -r answer; then
        echo >&2
    fi
    if [[ "$answer" != [yY] && "$answer" != [yY][eE][sS] ]]; then
        echo "Nothing changed."
        exit 1
    fi
    privileged nmcli con delete "$connection"
    echo "Removed $connection."
    exit 0
fi

if [[ -n "$interface" ]]; then
    check_interface "$interface"
else
    if ! interface="$(choose_interface)"; then
        exit 3
    fi
fi

if (( ${#addresses[@]} == 0 )); then
    print_state "$interface"
    echo "== Camera LAN interface: $interface"
    echo
    echo "Give the Jetson a free address in the cameras' subnet, for example:"
    echo "  make camera-lan-setup ADDRESS=192.168.10.5/24"
    exit 0
fi

for address in "${addresses[@]}"; do
    validate_address "$address"
done
# No two subnets may overlap: neither the new ones with each other nor with another interface's
# (the modem, Docker, the USB gadget), or traffic for one would leave through the other.
for (( i = 0; i < ${#addresses[@]}; i++ )); do
    for (( j = i + 1; j < ${#addresses[@]}; j++ )); do
        if subnets_overlap "${addresses[$i]}" "${addresses[$j]}"; then
            die 2 "${addresses[$i]} and ${addresses[$j]} overlap."
        fi
    done
    while read -r name; do
        if [[ "$name" == "$interface" ]]; then
            continue
        fi
        while read -r existing; do
            if [[ -n "$existing" ]] && subnets_overlap "${addresses[$i]}" "$existing"; then
                die 2 "${addresses[$i]} overlaps $(network_of "$existing") on $name" \
                    "($(interface_kind "$name")). Pick a camera subnet that no other interface" \
                    "uses."
            fi
        done <<< "$(addresses_of "$name")"
    done <<< "$(all_interfaces)"
done
address_list="$(IFS=,; echo "${addresses[*]}")"

add_command=(nmcli con add type ethernet ifname "$interface" con-name "$connection"
    ipv4.method manual ipv4.addresses "$address_list" ipv4.never-default yes
    ipv4.ignore-auto-dns yes ipv6.method ignore connection.autoconnect yes
    connection.autoconnect-priority 100)
up_command=(nmcli con up "$connection")
link="$(link_of "$interface")"

print_state "$interface"
echo "== Camera LAN interface: $interface (link $link)"

already=0
if (( have_nmcli && nm_running )) && connection_exists; then
    current_interface="$(connection_setting connection.interface-name)"
    current_addresses="$(connection_setting ipv4.addresses)"
    current_addresses="$(tr -d ' ' <<< "$current_addresses" | tr ',' '\n' | sort | paste -sd, -)"
    wanted_addresses="$(tr ',' '\n' <<< "$address_list" | sort | paste -sd, -)"
    if [[ "$current_interface" == "$interface" &&
          "$current_addresses" == "$wanted_addresses" ]]; then
        already=1
    else
        die 2 "$connection already exists (${current_addresses:-no address} on" \
            "${current_interface:-any interface}). Remove it first: make camera-lan-setup" \
            "LAN_ARGS=--remove"
    fi
fi

echo "== Commands"
if (( already )); then
    echo "  $connection already has $address_list on $interface; only activating it:"
else
    echo "  $sudo_word${add_command[*]}"
fi
if [[ "$link" == up ]]; then
    echo "  $sudo_word${up_command[*]}"
else
    echo "  ($interface has no link: NetworkManager activates the profile when the switch is" \
        "connected)"
fi

if (( ! apply )); then
    echo
    echo "Dry run: nothing was changed. To apply:"
    echo "  make camera-lan-setup INTERFACE=$interface ADDRESS=$address_list LAN_ARGS=--apply"
    exit 0
fi

(( have_nmcli )) || die 3 "nmcli is not installed. Configure the static address with the tool" \
    "that manages $interface instead (docs/CAMERAS.md)."
(( nm_running )) || die 3 "NetworkManager is not running, so a profile would not be applied."
device_state="$(nm_device_state "$interface")"
if [[ "$device_state" == unmanaged ]]; then
    die 3 "NetworkManager does not manage $interface (unmanaged-devices in" \
        "/etc/NetworkManager/NetworkManager.conf); configure it with the tool that does."
fi

echo
echo "WARNING: an SSH session over $interface may drop while NetworkManager switches it to" \
    "$connection; connect over the modem, Wi-Fi or a screen and keyboard if in doubt."
if [[ -n "${SSH_CONNECTION:-}" ]]; then
    read -r _ _ ssh_server _ <<< "$SSH_CONNECTION"
    while read -r existing; do
        if [[ -n "$existing" && "${existing%/*}" == "$ssh_server" ]]; then
            echo "WARNING: this SSH session itself runs over $interface ($ssh_server)." >&2
        fi
    done <<< "$(addresses_of "$interface")"
fi
printf 'Apply to %s? [y/N] ' "$interface" >&2
answer=""
if (( assume_yes )); then
    answer=y
    echo y >&2
elif ! IFS= read -r answer; then
    echo >&2
fi
if [[ "$answer" != [yY] && "$answer" != [yY][eE][sS] ]]; then
    echo "Nothing changed."
    exit 1
fi

routes_before="$(default_routes)"
if (( ! already )); then
    if ! privileged "${add_command[@]}"; then
        die 3 "nmcli could not add $connection; nothing was changed."
    fi
fi
if [[ "$link" == up ]] && ! privileged "${up_command[@]}"; then
    echo "ERROR: $connection did not activate on $interface; removing it again." >&2
    privileged nmcli con delete "$connection" || true
    exit 3
fi
sleep "$settle_seconds"
routes_after="$(default_routes)"
if [[ "$routes_after" != "$routes_before" ]]; then
    echo "ERROR: the default route changed, so Internet traffic would no longer use the modem." >&2
    echo "  before: $(route_summary "$routes_before")" >&2
    echo "  after:  $(route_summary "$routes_after")" >&2
    echo "Rolling back: $connection is taken down and deleted." >&2
    privileged nmcli con down "$connection" || true
    privileged nmcli con delete "$connection" || true
    sleep "$settle_seconds"
    echo "  now:    $(route_summary "$(default_routes)")" >&2
    exit 3
fi

echo
echo "Done: $interface has $address_list (profile $connection, kept across reboots)."
echo "Default routes unchanged: $(route_summary "$routes_after")"
echo "Next: make camera-scan"
