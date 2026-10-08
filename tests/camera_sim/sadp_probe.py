#!/usr/bin/env python3
"""Discovery client for the simulated camera LAN (tests/camera_sim): SADP inquiry, ONVIF
WS-Discovery Probe, and ONVIF GetDeviceInformation with a WS-UsernameToken.

Development tooling only. It sends what open-source Hikvision tools send (read-only inquiries)
and works against real cameras too, but its purpose is to check the simulator from a container on
the `kzcam` Docker network. Python 3.6+ standard library only. ONVIF credentials are read from
the environment, never from the command line, and never printed.

  sadp_probe.py sadp [--expect 192.168.77.21 ...]      SADP inquiry on 239.255.255.250:37020
  sadp_probe.py wsd  [--expect 192.168.77.22 ...]      WS-Discovery Probe on 239.255.255.250:3702
  sadp_probe.py onvif-info 192.168.77.22               GetSystemDateAndTime + GetDeviceInformation

Exit status: 0 when every --expect address answered (or, without --expect, when anything
answered), 1 otherwise; onvif-info returns 2 when the camera rejected the credentials.
"""

import argparse
import base64
import datetime
import hashlib
import json
import os
import socket
import sys
import time
import uuid
import xml.etree.ElementTree as ET
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

GROUP = "239.255.255.250"
SADP_PORT = 37020
WSD_PORT = 3702
SOAP12_NS = "http://www.w3.org/2003/05/soap-envelope"
WSSE_NS = ("http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-secext-1.0.xsd")
WSU_NS = ("http://docs.oasis-open.org/wss/2004/01/"
          "oasis-200401-wss-wssecurity-utility-1.0.xsd")
TOKEN_PROFILE = ("http://docs.oasis-open.org/wss/2004/01/"
                 "oasis-200401-wss-username-token-profile-1.0")


def local_name(tag):
    return tag.rsplit("}", 1)[-1] if isinstance(tag, str) else ""


def parse_xml(payload):
    start = payload.find(b"<")
    if start < 0:
        return None
    try:
        return ET.fromstring(payload[start:])
    except ET.ParseError:
        return None


def children_text(element):
    """{local name: text} of the direct children."""
    return {local_name(child.tag): (child.text or "").strip() for child in element}


# -- SADP --------------------------------------------------------------------------------------


def build_sadp_probe(probe_uuid, types="inquiry"):
    return ('<?xml version="1.0" encoding="utf-8"?><Probe><Uuid>%s</Uuid><Types>%s</Types>'
            '</Probe>' % (probe_uuid, types)).encode("utf-8")


def parse_sadp_reply(payload):
    """{field: value} of a <ProbeMatch>, None for anything else."""
    root = parse_xml(payload)
    if root is None or local_name(root.tag) != "ProbeMatch":
        return None
    return children_text(root)


# -- WS-Discovery ------------------------------------------------------------------------------


def build_ws_probe(message_id):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<e:Envelope xmlns:e="%s" xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing" '
        'xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery" '
        'xmlns:dn="http://www.onvif.org/ver10/network/wsdl">'
        '<e:Header><w:MessageID>urn:uuid:%s</w:MessageID>'
        '<w:To e:mustUnderstand="true">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>'
        '<w:Action e:mustUnderstand="true">'
        'http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header>'
        '<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body>'
        '</e:Envelope>' % (SOAP12_NS, message_id)).encode("utf-8")


def parse_ws_reply(payload):
    """A list of {endpoint, types, scopes, xaddrs, relates_to} per ProbeMatch."""
    root = parse_xml(payload)
    if root is None or local_name(root.tag) != "Envelope":
        return []
    relates_to = ""
    for item in root.iter():
        if local_name(item.tag) == "RelatesTo":
            relates_to = (item.text or "").strip()
    matches = []
    for item in root.iter():
        if local_name(item.tag) != "ProbeMatch":
            continue
        found = {"endpoint": "", "types": "", "scopes": [], "xaddrs": [],
                 "relates_to": relates_to}
        for field in item.iter():
            name = local_name(field.tag)
            text = (field.text or "").strip()
            if name == "Address":
                found["endpoint"] = text
            elif name == "Types":
                found["types"] = text
            elif name == "Scopes":
                found["scopes"] = text.split()
            elif name == "XAddrs":
                found["xaddrs"] = text.split()
        matches.append(found)
    return matches


def scope_value(scopes, kind):
    prefix = "onvif://www.onvif.org/%s/" % kind
    for scope in scopes:
        if scope.startswith(prefix):
            return scope[len(prefix):].replace("%20", " ")
    return ""


# -- UDP plumbing ------------------------------------------------------------------------------


def ip_key(text):
    try:
        return socket.inet_aton(text)
    except OSError:
        return b"\xff" * 4


def parse_target(text, default_port):
    host, _, port = text.partition(":")
    return host, int(port) if port else default_port


def open_probe_socket(bind_port, interface_ip):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("", bind_port))
    except OSError:
        sock.bind(("", 0))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    if interface_ip:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                        socket.inet_aton(interface_ip))
    return sock


def collect(sock, timeout):
    """Every (payload, sender) that arrives within `timeout` seconds."""
    deadline = time.monotonic() + timeout
    received = []
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return received
        sock.settimeout(remaining)
        try:
            received.append(sock.recvfrom(65535))
        except socket.timeout:
            return received


# -- commands ----------------------------------------------------------------------------------


def run_sadp(args):
    probe_uuid = str(uuid.uuid4()).upper()
    target = parse_target(args.target, SADP_PORT) if args.target else (GROUP, SADP_PORT)
    sock = open_probe_socket(args.bind_port, args.interface_ip)
    try:
        for types in ("inquiry", "inquiry_v32"):
            sock.sendto(build_sadp_probe(probe_uuid, types), target)
        replies = collect(sock, args.timeout)
    finally:
        sock.close()
    devices = {}
    for payload, sender in replies:
        fields = parse_sadp_reply(payload)
        if fields is None or fields.get("Uuid", "").upper() != probe_uuid:
            continue
        key = (fields.get("MAC", ""), fields.get("IPv4Address", ""))
        fields["_from"] = "%s:%d" % sender
        devices.setdefault(key, fields)
    found = sorted(devices.values(), key=lambda item: ip_key(item.get("IPv4Address", "")))
    if args.json:
        print(json.dumps(found, indent=2, sort_keys=True))
    else:
        print("SADP inquiry %s -> %d device(s)" % (probe_uuid, len(found)))
        for item in found:
            print("  %-15s mac=%s model=%s serial=%s activated=%s http=%s sdk=%s sw=%r from=%s"
                  % (item.get("IPv4Address"), item.get("MAC"), item.get("DeviceDescription"),
                     item.get("DeviceSN"), item.get("Activated"), item.get("HttpPort"),
                     item.get("CommandPort"), item.get("SoftwareVersion"), item["_from"]))
    return check_expected(args.expect, [item.get("IPv4Address", "") for item in found])


def run_wsd(args):
    message_id = str(uuid.uuid4())
    target = parse_target(args.target, WSD_PORT) if args.target else (GROUP, WSD_PORT)
    sock = open_probe_socket(0, args.interface_ip)
    try:
        sock.sendto(build_ws_probe(message_id), target)
        replies = collect(sock, args.timeout)
    finally:
        sock.close()
    found = []
    for payload, sender in replies:
        for match in parse_ws_reply(payload):
            if match["relates_to"] != "urn:uuid:" + message_id:
                continue
            match["from"] = sender[0]
            found.append(match)
    if args.json:
        print(json.dumps(found, indent=2, sort_keys=True))
    else:
        print("WS-Discovery Probe urn:uuid:%s -> %d match(es)" % (message_id, len(found)))
        for match in found:
            print("  %-15s endpoint=%s hardware=%s name=%r xaddrs=%s" % (
                match["from"], match["endpoint"], scope_value(match["scopes"], "hardware"),
                scope_value(match["scopes"], "name"), " ".join(match["xaddrs"])))
    return check_expected(args.expect, [match["from"] for match in found])


def check_expected(expected, answered):
    if not expected:
        return 0 if answered else 1
    missing = [ip for ip in expected if ip not in answered]
    if missing:
        print("MISSING: no answer from %s" % ", ".join(missing))
        return 1
    return 0


def soap_call(url, body, timeout):
    request = Request(url, data=body.encode("utf-8"), method="POST",
                      headers={"Content-Type": "application/soap+xml; charset=utf-8"})
    try:
        with urlopen(request, timeout=timeout) as response:
            return response.status, response.read()
    except HTTPError as error:
        return error.code, error.read()


def soap_request(inner, header=""):
    return ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="%s" '
            'xmlns:tds="http://www.onvif.org/ver10/device/wsdl"><s:Header>%s</s:Header>'
            '<s:Body>%s</s:Body></s:Envelope>' % (SOAP12_NS, header, inner))


def username_token(username, password, created):
    nonce = os.urandom(16)
    digest = base64.b64encode(hashlib.sha1(nonce + created.encode("utf-8") +
                                           password.encode("utf-8")).digest()).decode("ascii")
    return ('<Security s:mustUnderstand="1" xmlns="%s"><UsernameToken><Username>%s</Username>'
            '<Password Type="%s#PasswordDigest">%s</Password>'
            '<Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-'
            'message-security-1.0#Base64Binary">%s</Nonce><Created xmlns="%s">%s</Created>'
            '</UsernameToken></Security>'
            % (WSSE_NS, username, TOKEN_PROFILE, digest,
               base64.b64encode(nonce).decode("ascii"), WSU_NS, created))


def device_utc(payload):
    """The camera's UTC clock from a GetSystemDateAndTime answer, None when absent."""
    root = parse_xml(payload)
    if root is None:
        return None
    for item in root.iter():
        if local_name(item.tag) == "UTCDateTime":
            values = {local_name(field.tag): (field.text or "").strip() for field in item.iter()}
            try:
                return datetime.datetime(int(values["Year"]), int(values["Month"]),
                                         int(values["Day"]), int(values["Hour"]),
                                         int(values["Minute"]), int(values["Second"]))
            except (KeyError, ValueError):
                return None
    return None


def run_onvif_info(args):
    host, port = parse_target(args.host, 80)
    url = "http://%s:%d/onvif/device_service" % (host, port)
    try:
        status, payload = soap_call(url, soap_request("<tds:GetSystemDateAndTime/>"),
                                    args.timeout)
    except (URLError, OSError) as error:
        print("ONVIF %s: unreachable (%s)" % (host, error))
        return 1
    if status != 200:
        print("ONVIF %s: GetSystemDateAndTime HTTP %d (ONVIF disabled or unsupported)"
              % (host, status))
        return 1
    clock = device_utc(payload)
    print("ONVIF %s: GetSystemDateAndTime OK, camera UTC %s" % (host, clock))
    username = os.environ.get(args.username_env, "")
    password = os.environ.get(args.password_env, "")
    if not username or not password:
        print("ONVIF %s: %s/%s not set, GetDeviceInformation skipped"
              % (host, args.username_env, args.password_env))
        return 0
    created = (clock or datetime.datetime.utcnow()).strftime("%Y-%m-%dT%H:%M:%SZ")
    status, payload = soap_call(url, soap_request("<tds:GetDeviceInformation/>",
                                                  username_token(username, password, created)),
                                args.timeout)
    if status in (400, 401):
        print("ONVIF %s: GetDeviceInformation rejected the credentials (HTTP %d)"
              % (host, status))
        return 2
    root = parse_xml(payload)
    info = {}
    if root is not None:
        for item in root.iter():
            if local_name(item.tag) == "GetDeviceInformationResponse":
                info = children_text(item)
    if status != 200 or not info:
        print("ONVIF %s: GetDeviceInformation HTTP %d without device information"
              % (host, status))
        return 1
    print("ONVIF %s: GetDeviceInformation OK manufacturer=%s model=%s firmware=%r serial=%s"
          % (host, info.get("Manufacturer"), info.get("Model"), info.get("FirmwareVersion"),
             info.get("SerialNumber")))
    return 0


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command")
    for name, help_text in (("sadp", "SADP inquiry"), ("wsd", "ONVIF WS-Discovery Probe")):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("--timeout", type=float, default=3.0, help="listen seconds")
        command.add_argument("--target", default="",
                             help="unicast HOST[:PORT] instead of the multicast group")
        command.add_argument("--interface-ip", default="",
                             help="send multicast out of the interface with this address")
        command.add_argument("--expect", nargs="*", default=[],
                             help="addresses that must answer")
        command.add_argument("--json", action="store_true")
        if name == "sadp":
            command.add_argument("--bind-port", type=int, default=SADP_PORT,
                                 help="source port (devices answer to it; 37020 like SADP)")
    info = commands.add_parser("onvif-info", help="ONVIF device information")
    info.add_argument("host", help="HOST[:PORT] of the camera's HTTP service")
    info.add_argument("--timeout", type=float, default=4.0)
    info.add_argument("--username-env", default="ONVIF_USERNAME")
    info.add_argument("--password-env", default="ONVIF_PASSWORD")
    return parser


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0].startswith("-") and argv[0] not in ("-h", "--help"):
        argv.insert(0, "sadp")
    args = build_parser().parse_args(argv)
    if args.command == "wsd":
        return run_wsd(args)
    if args.command == "onvif-info":
        return run_onvif_info(args)
    return run_sadp(args)


if __name__ == "__main__":
    sys.exit(main())
