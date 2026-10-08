#!/usr/bin/env python3
"""SIMULATION ONLY: the network services of a Hikvision IP camera, for development tests.

One process plays one camera: SADP and ONVIF WS-Discovery answers, the ISAPI resources the ANPR
tool reads, the ONVIF device service, the ISAPI alertStream with a fixed ANPR plate, and the SDK
port. Authentication follows the camera's rules (HTTP digest MD5 qop=auth, a separate ONVIF
account with WS-UsernameToken, an illegal-login lock per client IP) so tests can prove that the
tool never sends a wrong password twice. Video comes from a MediaMTX process that shares this
process's network namespace (tests/camera_sim/sim.sh).

Formats follow published captures of Hikvision firmware, not the firmware: passing against this
simulator never proves real-hardware behaviour. Python 3.6+ standard library only (Ubuntu 18.04
on the Jetson ships 3.6); no credentials are ever written to the log.
"""

import argparse
import base64
import collections
import datetime
import hashlib
import hmac
import http.server
import logging
import os
import queue
import re
import secrets
import signal
import socket
import socketserver
import struct
import sys
import threading
import time
import uuid
import xml.etree.ElementTree as ET
from urllib.parse import urlsplit
from xml.sax.saxutils import escape

SADP_GROUP = "239.255.255.250"
SADP_PORT = 37020
WSD_GROUP = "239.255.255.250"
WSD_PORT = 3702
HIK_NS = "http://www.hikvision.com/ver20/XMLSchema"
SOAP12_NS = "http://www.w3.org/2003/05/soap-envelope"
WSA_NS = "http://schemas.xmlsoap.org/ws/2004/08/addressing"
WSD_NS = "http://schemas.xmlsoap.org/ws/2005/04/discovery"
WSSE_PASSWORD_DIGEST = (
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0"
    "#PasswordDigest")
ALERT_BOUNDARY = "boundary"
MAX_DATAGRAM = 65535
MAX_REQUEST_BODY = 1 << 20
# How often blocking sockets wake up to notice a stop request.
POLL_SECONDS = 0.25

LOG = logging.getLogger("fake_hikvision")


# ---------------------------------------------------------------------------------------------
# Small helpers


def local_name(tag):
    """Element name without its {namespace}."""
    return tag.rsplit("}", 1)[-1] if isinstance(tag, str) else ""


def find_local(element, name):
    """First descendant (or the element itself) whose local name is `name`."""
    for item in element.iter():
        if local_name(item.tag) == name:
            return item
    return None


def text_of(element, name, default=""):
    found = find_local(element, name) if element is not None else None
    if found is None or found.text is None:
        return default
    return found.text.strip()


def parse_xml_bytes(payload):
    """ElementTree root of `payload`, skipping any binary prefix before the first '<' (some SADP
    senders put a short header in front of the XML). None when it is not XML."""
    if isinstance(payload, str):
        payload = payload.encode("utf-8", "replace")
    start = payload.find(b"<")
    if start < 0:
        return None
    try:
        return ET.fromstring(payload[start:])
    except ET.ParseError:
        return None


def xml_doc(root_name, body, version="2.0", namespace=HIK_NS):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<%s version="%s" xmlns="%s">\n%s</%s>\n'
            % (root_name, version, namespace, body, root_name))


def elements(pairs):
    """'<a>1</a>\\n<b>2</b>\\n' from [(name, value)], values escaped (nested XML passes as
    an already-built string through `raw()`)."""
    out = []
    for name, value in pairs:
        if isinstance(value, Raw):
            out.append("<%s>%s</%s>\n" % (name, value.text, name))
        else:
            out.append("<%s>%s</%s>\n" % (name, escape(str(value)), name))
    return "".join(out)


class Raw(object):
    """Marks pre-built XML for `elements`."""

    def __init__(self, text):
        self.text = text


def bool_text(value):
    return "true" if value else "false"


def parse_bool(text):
    lowered = str(text).strip().lower()
    if lowered in ("1", "true", "yes", "on"):
        return True
    if lowered in ("0", "false", "no", "off"):
        return False
    raise argparse.ArgumentTypeError("expected true/false or on/off, got %r" % text)


def md5_hex(text):
    return hashlib.md5(text.encode("utf-8")).hexdigest()


def normalize_mac(mac, separator=":"):
    digits = re.sub(r"[^0-9a-fA-F]", "", mac or "").lower()
    if len(digits) != 12:
        raise ValueError("not a MAC address: %r" % mac)
    return separator.join(digits[i:i + 2] for i in range(0, 12, 2))


def parse_utc_offset(text):
    match = re.match(r"^([+-])(\d{2}):?(\d{2})$", text.strip())
    if not match:
        raise argparse.ArgumentTypeError("UTC offset like +05:00 expected, got %r" % text)
    minutes = int(match.group(2)) * 60 + int(match.group(3))
    return datetime.timezone(datetime.timedelta(minutes=-minutes if match.group(1) == "-"
                                                else minutes))


def iso_with_offset(moment, tz):
    """2026-10-09T01:02:03+05:00, the dateTime format of Hikvision events."""
    text = moment.astimezone(tz).strftime("%Y-%m-%dT%H:%M:%S%z")
    return text[:-2] + ":" + text[-2:]


def parse_soap_datetime(text):
    """UTC datetime from an xs:dateTime as WS-Security Created uses it; None when malformed."""
    match = re.match(r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(\.\d+)?"
                     r"(Z|[+-]\d{2}:\d{2})?$", (text or "").strip())
    if not match:
        return None
    try:
        moment = datetime.datetime(*[int(match.group(i)) for i in range(1, 7)])
    except ValueError:
        return None
    if match.group(7):
        moment = moment.replace(microsecond=int(float(match.group(7)) * 1000000))
    zone = match.group(8)
    if zone and zone != "Z":
        sign = -1 if zone[0] == "-" else 1
        offset = datetime.timedelta(hours=int(zone[1:3]), minutes=int(zone[4:6]))
        moment = moment - sign * offset
    return moment.replace(tzinfo=datetime.timezone.utc)


def onvif_password_digest(nonce_raw, created, password):
    """Base64(SHA1(nonce + created + password)), the WS-Security PasswordDigest."""
    digest = hashlib.sha1(nonce_raw + created.encode("utf-8") + password.encode("utf-8"))
    return base64.b64encode(digest.digest()).decode("ascii")


# ---------------------------------------------------------------------------------------------
# Network defaults (Linux sysfs/procfs; explicit flags elsewhere)


def interface_mac(interface):
    try:
        with open("/sys/class/net/%s/address" % interface) as handle:
            return normalize_mac(handle.read().strip())
    except (OSError, ValueError):
        return None


def _ioctl_ipv4(interface, request):
    try:
        import fcntl
    except ImportError:
        return None
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        packed = struct.pack("256s", interface[:15].encode("ascii"))
        result = fcntl.ioctl(sock.fileno(), request, packed)
        return socket.inet_ntoa(result[20:24])
    except OSError:
        return None
    finally:
        sock.close()


def interface_ipv4(interface):
    return _ioctl_ipv4(interface, 0x8915)  # SIOCGIFADDR


def interface_netmask(interface):
    return _ioctl_ipv4(interface, 0x891B)  # SIOCGIFNETMASK


def default_gateway(interface):
    """Gateway of the default route through `interface`, from /proc/net/route."""
    try:
        with open("/proc/net/route") as handle:
            next(handle)
            for line in handle:
                fields = line.split()
                if len(fields) < 8 or fields[0] != interface:
                    continue
                if int(fields[1], 16) == 0 and int(fields[7], 16) == 0:
                    return socket.inet_ntoa(struct.pack("<I", int(fields[2], 16)))
    except (OSError, ValueError, StopIteration):
        pass
    return None


# ---------------------------------------------------------------------------------------------
# Configuration


class Channel(object):
    """One ISAPI streaming channel: what GET /ISAPI/Streaming/channels/<id> reports."""

    def __init__(self, channel_id, codec, width, height, max_frame_rate, gov_length):
        self.channel_id = channel_id
        self.codec = codec
        self.width = width
        self.height = height
        self.max_frame_rate = max_frame_rate  # hundredths of a frame per second
        self.gov_length = gov_length

    @classmethod
    def parse(cls, text):
        """'101,H.264,608x1080,3000[,160]': id, codec, size, centi-fps, I-frame interval."""
        parts = [part.strip() for part in text.split(",")]
        if len(parts) not in (4, 5):
            raise argparse.ArgumentTypeError(
                "channel must be ID,CODEC,WIDTHxHEIGHT,CENTI_FPS[,GOV], got %r" % text)
        codecs = {"h.264": "H.264", "h264": "H.264", "h.265": "H.265", "h265": "H.265",
                  "hevc": "H.265", "mjpeg": "MJPEG"}
        codec = codecs.get(parts[1].lower())
        size = re.match(r"^(\d+)x(\d+)$", parts[2])
        if not codec or not size or not parts[0].isdigit() or not parts[3].isdigit():
            raise argparse.ArgumentTypeError("invalid channel %r" % text)
        gov = int(parts[4]) if len(parts) == 5 and parts[4].isdigit() else 50
        return cls(int(parts[0]), codec, int(size.group(1)), int(size.group(2)), int(parts[3]),
                   gov)


class CameraConfig(object):
    """Everything one simulated camera reports and enforces."""

    def __init__(self, **values):
        self.name = "cam"
        self.model = "DS-TCG406-E"
        self.serial = ""
        self.mac = "bc:ad:28:00:00:01"
        self.ip = "127.0.0.1"
        self.netmask = "255.255.255.0"
        self.gateway = "0.0.0.0"
        self.bind = "0.0.0.0"
        self.http_port = 80
        self.rtsp_port = 554
        self.sdk_port = 8000
        self.sadp_port = SADP_PORT
        self.wsd_port = WSD_PORT
        self.multicast = True
        self.activated = True
        self.onvif = False
        self.username = "admin"
        self.password = ""
        self.onvif_username = ""
        self.onvif_password = ""
        self.realm = ""
        self.device_name = ""
        self.firmware = "V5.7.10"
        self.firmware_build = "build 231010"
        self.dsp_version = "V7.3"
        self.sadp_device_type = "138401"
        self.channels = []
        self.anpr_plate = "152JTA02"
        self.anpr_country = 30
        self.anpr_interval = 20.0
        self.anpr_pictures = []
        self.anpr_plate_rect = (338, 338, 117, 44)
        self.heartbeat_interval = 5.0
        self.lockout_attempts = 5
        self.lockout_window = 1800.0
        self.lockout_duration = 1800.0
        self.nonce_lifetime = 300.0
        self.onvif_clock_skew = 300.0
        self.rtsp_log = ""
        self.utc_offset = datetime.timezone(datetime.timedelta(hours=5))
        for key, value in values.items():
            if not hasattr(self, key):
                raise TypeError("unknown camera setting %r" % key)
            setattr(self, key, value)
        self.mac = normalize_mac(self.mac)
        last_octet = self.ip.rsplit(".", 1)[-1]
        code = "SIM%02d" % (int(last_octet) % 100 if last_octet.isdigit() else 0)
        if not self.serial:
            self.serial = "%s20260101AAWR%s%07d" % (
                self.model, code, int(self.mac.replace(":", "")[-6:], 16) % 10000000)
        if not self.realm:
            self.realm = "IP Camera(%s)" % code
        if not self.device_name:
            self.device_name = "SIM %s" % self.name
        if not self.onvif_username:
            self.onvif_username = self.username
            self.onvif_password = self.password
        if not self.channels:
            self.channels = [Channel(101, "H.264", 2688, 1520, 2500, 50),
                             Channel(102, "H.264", 640, 360, 2500, 50)]

    @property
    def device_id(self):
        return str(uuid.uuid5(uuid.NAMESPACE_URL, "sim-hikvision:" + self.serial))

    @property
    def onvif_endpoint_uuid(self):
        """Hikvision ends its ONVIF endpoint UUID with the MAC address."""
        digits = uuid.uuid5(uuid.NAMESPACE_URL, "sim-onvif:" + self.serial).hex
        mac = self.mac.replace(":", "")
        return "%s-%s-%s-%s-%s" % (digits[:8], digits[8:12], digits[12:16], digits[16:20], mac)

    def channel(self, channel_id):
        for item in self.channels:
            if item.channel_id == channel_id:
                return item
        return None


# ---------------------------------------------------------------------------------------------
# Authentication: digest, WS-UsernameToken, the illegal-login lock


class LoginGuard(object):
    """Hikvision's "illegal login lock": `attempts` failed logins from one IP within `window`
    seconds lock that IP out for `duration` seconds, whatever it sends meanwhile. Failures from
    every protocol (ISAPI, ONVIF, RTSP via the MediaMTX log) share one budget, as the defensive
    reading of the firmware documentation suggests; a correct login does not reset it."""

    def __init__(self, attempts, window, duration, clock=time.monotonic):
        self.attempts = attempts
        self.window = window
        self.duration = duration
        self._clock = clock
        self._failures = collections.defaultdict(collections.deque)
        self._locked_until = {}
        self._lock = threading.Lock()

    def locked_remaining(self, ip):
        with self._lock:
            until = self._locked_until.get(ip)
            if until is None:
                return 0.0
            remaining = until - self._clock()
            if remaining <= 0:
                del self._locked_until[ip]
                self._failures.pop(ip, None)
                return 0.0
            return remaining

    def failures(self, ip):
        with self._lock:
            self._expire(ip, self._clock())
            return len(self._failures.get(ip, ()))

    def record_failure(self, ip, source):
        """Counts one failed login; returns (failures in window, True when this one locked)."""
        with self._lock:
            now = self._clock()
            if ip in self._locked_until and self._locked_until[ip] > now:
                return len(self._failures[ip]), False
            self._expire(ip, now)
            self._failures[ip].append(now)
            count = len(self._failures[ip])
            if self.attempts > 0 and count >= self.attempts:
                self._locked_until[ip] = now + self.duration
                LOG.warning("LOCKED ip=%s after %d failed logins within %ds (last via %s); every "
                            "authenticated request from it gets 401 for %ds", ip, count,
                            int(self.window), source, int(self.duration))
                return count, True
            return count, False

    def _expire(self, ip, now):
        failures = self._failures.get(ip)
        while failures and now - failures[0] > self.window:
            failures.popleft()


class DigestResult(object):
    OK = "ok"
    NO_CREDENTIALS = "no-credentials"
    STALE = "stale-nonce"
    FAILED = "failed"

    def __init__(self, status, username="", reason=""):
        self.status = status
        self.username = username
        self.reason = reason


_AUTH_PARAM = re.compile(r'([A-Za-z0-9_-]+)\s*=\s*(?:"((?:[^"\\]|\\.)*)"|([^\s,]+))')


def parse_auth_params(text):
    params = {}
    for match in _AUTH_PARAM.finditer(text):
        value = match.group(2) if match.group(2) is not None else match.group(3)
        params[match.group(1).lower()] = re.sub(r"\\(.)", r"\1", value)
    return params


class DigestAuthenticator(object):
    """RFC 2617 digest, MD5 with qop=auth, as Hikvision ISAPI offers it (Basic is off by
    default on the cameras, so a Basic header is a failed login)."""

    def __init__(self, realm, users, nonce_lifetime, clock=time.monotonic):
        self.realm = realm
        self._users = dict(users)
        self._nonce_lifetime = nonce_lifetime
        self._clock = clock
        self._nonces = {}
        self._lock = threading.Lock()

    def challenge(self, stale=False):
        nonce = secrets.token_hex(24)
        with self._lock:
            now = self._clock()
            for old in [key for key, issued in self._nonces.items()
                        if now - issued > 2 * self._nonce_lifetime]:
                del self._nonces[old]
            self._nonces[nonce] = now
        return 'Digest qop="auth", realm="%s", nonce="%s", stale="%s"' % (
            self.realm, nonce, "TRUE" if stale else "FALSE")

    def verify(self, method, request_uri, header):
        if not header:
            return DigestResult(DigestResult.NO_CREDENTIALS)
        scheme, _, rest = header.strip().partition(" ")
        if scheme.lower() != "digest":
            return DigestResult(DigestResult.FAILED, reason="scheme-%s" % scheme.lower())
        params = parse_auth_params(rest)
        username = params.get("username", "")
        problems = []
        if username not in self._users:
            problems.append("unknown-user")
        if params.get("realm") != self.realm:
            problems.append("realm")
        if params.get("uri") != request_uri:
            problems.append("uri")
        if params.get("algorithm", "MD5").upper() != "MD5":
            problems.append("algorithm")
        if params.get("qop") != "auth" or not params.get("nc") or not params.get("cnonce"):
            problems.append("qop")
        nonce = params.get("nonce", "")
        if not nonce or not params.get("response"):
            problems.append("incomplete")
        if problems:
            return DigestResult(DigestResult.FAILED, username, ",".join(problems))
        ha1 = md5_hex("%s:%s:%s" % (username, self.realm, self._users[username]))
        ha2 = md5_hex("%s:%s" % (method, request_uri))
        expected = md5_hex("%s:%s:%s:%s:auth:%s" % (ha1, nonce, params["nc"], params["cnonce"],
                                                     ha2))
        if not hmac.compare_digest(expected, params["response"].lower()):
            return DigestResult(DigestResult.FAILED, username, "wrong-response")
        with self._lock:
            issued = self._nonces.get(nonce)
        if issued is None or self._clock() - issued > self._nonce_lifetime:
            return DigestResult(DigestResult.STALE, username)
        return DigestResult(DigestResult.OK, username)


class UsernameTokenResult(object):
    OK = "ok"
    NO_CREDENTIALS = "no-credentials"
    FAILED = "failed"

    def __init__(self, status, username="", reason=""):
        self.status = status
        self.username = username
        self.reason = reason


def verify_username_token(envelope, username, password, max_skew, now=None):
    """Checks the WS-Security UsernameToken of a SOAP envelope (PasswordDigest only)."""
    token = find_local(envelope, "UsernameToken")
    if token is None:
        return UsernameTokenResult(UsernameTokenResult.NO_CREDENTIALS)
    given_user = text_of(token, "Username")
    password_element = find_local(token, "Password")
    nonce_text = text_of(token, "Nonce")
    created = text_of(token, "Created")
    if password_element is None:
        return UsernameTokenResult(UsernameTokenResult.FAILED, given_user, "no-password")
    if not password_element.get("Type", "").endswith("#PasswordDigest"):
        return UsernameTokenResult(UsernameTokenResult.FAILED, given_user, "not-digest")
    try:
        nonce_raw = base64.b64decode(nonce_text.encode("ascii"), validate=True)
    except (ValueError, UnicodeEncodeError):
        return UsernameTokenResult(UsernameTokenResult.FAILED, given_user, "nonce")
    created_at = parse_soap_datetime(created)
    if created_at is None:
        return UsernameTokenResult(UsernameTokenResult.FAILED, given_user, "created")
    now = now or datetime.datetime.now(datetime.timezone.utc)
    if abs((now - created_at).total_seconds()) > max_skew:
        return UsernameTokenResult(UsernameTokenResult.FAILED, given_user, "clock-skew")
    if given_user != username:
        return UsernameTokenResult(UsernameTokenResult.FAILED, given_user, "unknown-user")
    expected = onvif_password_digest(nonce_raw, created, password)
    if not hmac.compare_digest(expected, (password_element.text or "").strip()):
        return UsernameTokenResult(UsernameTokenResult.FAILED, given_user, "wrong-digest")
    return UsernameTokenResult(UsernameTokenResult.OK, given_user)


# ---------------------------------------------------------------------------------------------
# Documents


def sadp_probe_match(config, probe_uuid, boot_time, http_port, sdk_port):
    """The <ProbeMatch> answer to an SADP inquiry: exactly the fields that open-source SADP
    tools read from Hikvision firmware (research/hikvision.md section 1), in that order."""
    fields = [
        ("Uuid", probe_uuid),
        ("Types", "inquiry"),
        ("DeviceType", config.sadp_device_type),
        ("DeviceDescription", config.model),
        ("DeviceSN", config.serial),
        ("MAC", config.mac.replace(":", "-")),
        ("IPv4Address", config.ip),
        ("IPv4SubnetMask", config.netmask),
        ("IPv4Gateway", config.gateway),
        ("IPv6Address", "::"),
        ("IPv6Gateway", "::"),
        ("IPv6MaskLen", 64),
        ("DHCP", "false"),
        ("CommandPort", sdk_port),
        ("HttpPort", http_port),
        ("DSPVersion", "%s %s" % (config.dsp_version, config.firmware_build)),
        ("BootTime", boot_time),
        ("SoftwareVersion", "%s%s" % (config.firmware, config.firmware_build)),
        ("Activated", bool_text(config.activated)),
        ("PasswordResetModeSecond", "true"),
        ("PasswordResetAbility", "true"),
        ("SupportSecurityQuestion", "true"),
        ("SupportHCPlatform", "true"),
        ("HCPlatformEnable", "false"),
        ("Encoder", "true"),
        ("OEMInfo", "SIMULATED"),
        ("AnalogChannelNum", 0),
        ("DigitalChannelNum", 1),
        ("SDKOverTLSPort", 8443),
        ("SDKServerStatus", "true"),
    ]
    return ('<?xml version="1.0" encoding="UTF-8"?><ProbeMatch>%s</ProbeMatch>'
            % elements(fields).replace("\n", ""))


def ws_discovery_probe_match(config, relates_to, http_port):
    host = config.ip if http_port == 80 else "%s:%d" % (config.ip, http_port)
    model = escape(config.model)
    scopes = " ".join([
        "onvif://www.onvif.org/type/video_encoder",
        "onvif://www.onvif.org/Profile/Streaming",
        "onvif://www.onvif.org/Profile/G",
        "onvif://www.onvif.org/Profile/T",
        "onvif://www.onvif.org/hardware/%s" % model,
        "onvif://www.onvif.org/name/HIKVISION%%20%s" % model,
        "onvif://www.onvif.org/location/city/hangzhou",
    ])
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<env:Envelope xmlns:env="%s" xmlns:wsa="%s" xmlns:d="%s" '
        'xmlns:dn="http://www.onvif.org/ver10/network/wsdl" '
        'xmlns:tds="http://www.onvif.org/ver10/device/wsdl">'
        '<env:Header>'
        '<wsa:MessageID>urn:uuid:%s</wsa:MessageID>'
        '<wsa:RelatesTo>%s</wsa:RelatesTo>'
        '<wsa:To env:mustUnderstand="true">'
        'http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>'
        '<wsa:Action env:mustUnderstand="true">'
        'http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>'
        '<d:AppSequence InstanceId="1" MessageNumber="1"/>'
        '</env:Header>'
        '<env:Body><d:ProbeMatches><d:ProbeMatch>'
        '<wsa:EndpointReference><wsa:Address>urn:uuid:%s</wsa:Address></wsa:EndpointReference>'
        '<d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types>'
        '<d:Scopes>%s</d:Scopes>'
        '<d:XAddrs>http://%s/onvif/device_service</d:XAddrs>'
        '<d:MetadataVersion>10</d:MetadataVersion>'
        '</d:ProbeMatch></d:ProbeMatches></env:Body></env:Envelope>'
        % (SOAP12_NS, WSA_NS, WSD_NS, uuid.uuid4(), escape(relates_to),
           config.onvif_endpoint_uuid, scopes, host))


def device_info_xml(config):
    return xml_doc("DeviceInfo", elements([
        ("deviceName", config.device_name),
        ("deviceID", config.device_id),
        ("deviceDescription", "IPCamera"),
        ("deviceLocation", "hangzhou"),
        ("systemContact", "SIMULATED camera (tests/camera_sim)"),
        ("model", config.model),
        ("serialNumber", config.serial),
        ("macAddress", config.mac),
        ("firmwareVersion", config.firmware),
        ("firmwareReleasedDate", config.firmware_build),
        ("encoderVersion", config.dsp_version),
        ("encoderReleasedDate", config.firmware_build),
        ("bootVersion", "V1.3.4"),
        ("bootReleasedDate", "100316"),
        ("hardwareVersion", "0x0"),
        ("deviceType", "IPCamera"),
        ("telecontrolID", 88),
        ("supportBeep", "false"),
        ("supportVideoLoss", "false"),
        ("firmwareVersionInfo", "B-R-SIM-0"),
        ("manufacturer", "hikvision"),
    ]))


def streaming_channel_body(config, channel):
    rate_control = "VBR"
    video = [
        ("enabled", "true"),
        ("videoInputChannelID", 1),
        ("videoCodecType", channel.codec),
        ("videoScanType", "progressive"),
        ("videoResolutionWidth", channel.width),
        ("videoResolutionHeight", channel.height),
        ("videoQualityControlType", rate_control),
        ("constantBitRate", 4096),
        ("fixedQuality", 60),
        ("vbrUpperCap", 4096),
        ("vbrLowerCap", 32),
        ("maxFrameRate", channel.max_frame_rate),
        ("keyFrameInterval", int(channel.gov_length * 100000 // max(channel.max_frame_rate, 1))),
        ("snapShotImageType", "JPEG"),
        ("GovLength", channel.gov_length),
        ("SVC", Raw("<enabled>false</enabled>")),
        ("smoothing", 50),
    ]
    if channel.codec == "H.264":
        video.append(("H264Profile", "High"))
    elif channel.codec == "H.265":
        video.append(("H265Profile", "Main"))
    transport = elements([
        ("maxPacketSize", 1000),
        ("ControlProtocolList", Raw(
            "<ControlProtocol><streamingTransport>RTSP</streamingTransport></ControlProtocol>"
            "<ControlProtocol><streamingTransport>HTTP</streamingTransport></ControlProtocol>")),
        ("Unicast", Raw("<enabled>true</enabled><rtpTransportType>RTP/TCP</rtpTransportType>")),
        ("Security", Raw("<enabled>true</enabled><certificateType>digest</certificateType>")),
    ])
    return elements([
        ("id", channel.channel_id),
        ("channelName", config.device_name),
        ("enabled", "true"),
        ("Transport", Raw("\n" + transport)),
        ("Video", Raw("\n" + elements(video))),
        ("Audio", Raw("<enabled>false</enabled><audioInputChannelID>1</audioInputChannelID>"
                      "<audioCompressionType>G.711ulaw</audioCompressionType>")),
    ])


def streaming_channel_xml(config, channel):
    return xml_doc("StreamingChannel", streaming_channel_body(config, channel))


def streaming_channel_list_xml(config):
    body = "".join('<StreamingChannel version="2.0" xmlns="%s">\n%s</StreamingChannel>\n'
                   % (HIK_NS, streaming_channel_body(config, channel))
                   for channel in config.channels)
    return xml_doc("StreamingChannelList", body)


def traffic_capabilities_xml(config):
    return xml_doc("TrafficCap", elements([
        ("isSupportVehicleDetection", "true"),
        ("isSupportVehicleDetect", "true"),
        ("isSupportANPR", "true"),
        ("ANPR", Raw("<isSupport>true</isSupport><supportCountry>%d</supportCountry>"
                     % config.anpr_country)),
        ("VehicleDetect", Raw("<isSupport>true</isSupport><channelIDList>1</channelIDList>")),
    ]))


def network_interfaces_xml(config):
    return xml_doc("NetworkInterfaceList", elements([("NetworkInterface", Raw(
        "<id>1</id><IPAddress><ipVersion>v4</ipVersion><addressingType>static</addressingType>"
        "<ipAddress>%s</ipAddress><subnetMask>%s</subnetMask><DefaultGateway><ipAddress>%s"
        "</ipAddress></DefaultGateway></IPAddress><Link><MACAddress>%s</MACAddress>"
        "<autoNegotiation>true</autoNegotiation><speed>100</speed><duplex>full</duplex>"
        "<MTU>1500</MTU></Link>" % (config.ip, config.netmask, config.gateway, config.mac)))]))


def integrate_xml(config):
    return xml_doc("Integrate", elements([
        ("CGI", Raw("<enable>false</enable><certificateType>digest</certificateType>")),
        ("ONVIF", Raw("<enable>%s</enable>" % bool_text(config.onvif))),
        ("ISAPI", Raw("<enable>true</enable>")),
    ]))


def time_xml(config):
    now = datetime.datetime.now(datetime.timezone.utc)
    return xml_doc("Time", elements([
        ("timeMode", "manual"),
        ("localTime", iso_with_offset(now, config.utc_offset)),
        ("timeZone", "CST%s" % posix_offset(config.utc_offset)),
    ]))


def posix_offset(tz):
    """'-5:00:00' for UTC+05:00: POSIX TZ offsets are west-positive, Hikvision's CST-8:00:00."""
    minutes = int(tz.utcoffset(None).total_seconds() // 60)
    sign = "-" if minutes >= 0 else "+"
    minutes = abs(minutes)
    return "%s%d:%02d:00" % (sign, minutes // 60, minutes % 60)


def user_check_xml(status_value, locked_seconds, retries_left, activated=True):
    return xml_doc("userCheck", elements([
        ("statusValue", status_value),
        ("statusString", "OK" if status_value == 200 else "Unauthorized"),
        ("isDefaultPassword", "false"),
        ("isRiskPassword", "false"),
        ("isActivated", bool_text(activated)),
        ("lockStatus", "lock" if locked_seconds > 0 else "unlock"),
        ("unlockTime", int(locked_seconds)),
        ("retryLoginTime", max(retries_left, 0)),
    ]))


def response_status_xml(request_url, status_code, status_string, sub_status):
    return xml_doc("ResponseStatus", elements([
        ("requestURL", request_url),
        ("statusCode", status_code),
        ("statusString", status_string),
        ("subStatusCode", sub_status),
    ]))


def heartbeat_alert_xml(config, http_port, now=None):
    now = now or datetime.datetime.now(datetime.timezone.utc)
    return xml_doc("EventNotificationAlert", elements([
        ("ipAddress", config.ip),
        ("portNo", http_port),
        ("protocol", "HTTP"),
        ("macAddress", config.mac),
        ("channelID", 1),
        ("dateTime", iso_with_offset(now, config.utc_offset)),
        ("activePostCount", 0),
        ("eventType", "videoloss"),
        ("eventState", "inactive"),
        ("eventDescription", "videoloss alarm"),
    ]))


class AnprEvent(object):
    """One plate read by the (simulated) camera, shared by every connected alertStream."""

    def __init__(self, plate, index, moment, event_uuid):
        self.plate = plate
        self.index = index
        self.moment = moment
        self.uuid = event_uuid


def anpr_alert_xml(config, http_port, event, picture_names):
    """EventNotificationAlert with the <ANPR> block in the shape of a captured Hikvision ANPR
    event (country 30 = Kazakhstan)."""
    abs_time = event.moment.astimezone(config.utc_offset).strftime("%Y%m%d%H%M%S") + "%03d" % (
        event.moment.microsecond // 1000)
    pictures = []
    for index, name in enumerate(picture_names):
        kind = name.rsplit(".", 1)[0]
        rect = ""
        if kind in ("vehiclePicture", "detectionPicture"):
            rect = ("<plateRect><X>%d</X><Y>%d</Y><width>%d</width><height>%d</height>"
                    "</plateRect>" % tuple(config.anpr_plate_rect))
        pictures.append(
            "<pictureInfo><fileName>%s</fileName><type>%s</type><dataType>0</dataType>"
            "<absTime>%s</absTime><pId>%s%02d</pId>%s</pictureInfo>"
            % (name, kind, abs_time, event.uuid.replace("-", "")[:16], index + 1, rect))
    believe = ",".join(str(99 - (i % 3)) for i in range(len(event.plate)))
    anpr = elements([
        ("country", config.anpr_country),
        ("licensePlate", event.plate),
        ("line", 1),
        ("direction", "forward"),
        ("confidenceLevel", 98),
        ("plateType", "unknown"),
        ("plateColor", "white"),
        ("licenseBright", 0),
        ("vehicleType", "vehicle"),
        ("plateCharBelieve", believe),
        ("vehicleInfo", Raw("<index>%d</index><colorDepth>2</colorDepth><color>white</color>"
                            "<vehicleLogoRecog>0</vehicleLogoRecog>" % event.index)),
        ("pictureInfoList", Raw("".join(pictures))),
        ("originalLicensePlate", event.plate),
        ("CRIndex", config.anpr_country),
        ("vehicleListName", "otherList"),
    ])
    return xml_doc("EventNotificationAlert", elements([
        ("ipAddress", config.ip),
        ("portNo", http_port),
        ("protocol", "HTTP"),
        ("macAddress", config.mac),
        ("channelID", 1),
        ("dateTime", iso_with_offset(event.moment, config.utc_offset)),
        ("activePostCount", 1),
        ("eventType", "ANPR"),
        ("eventState", "active"),
        ("eventDescription", "ANPR"),
        ("channelName", config.device_name),
        ("ANPR", Raw("\n" + anpr)),
        ("UUID", event.uuid),
        ("picNum", len(picture_names)),
    ]))


def multipart_part(headers, body):
    """One alertStream part: the delimiter line, headers, a blank line, the body, CRLF."""
    if isinstance(body, str):
        body = body.encode("utf-8")
    head = "--%s\r\n" % ALERT_BOUNDARY
    for name, value in headers:
        head += "%s: %s\r\n" % (name, value)
    head += "Content-Length: %d\r\n\r\n" % len(body)
    return head.encode("ascii") + body + b"\r\n"


def soap_envelope(body):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<env:Envelope xmlns:env="%s" xmlns:tds="http://www.onvif.org/ver10/device/wsdl" '
            'xmlns:tt="http://www.onvif.org/ver10/schema" '
            'xmlns:ter="http://www.onvif.org/ver10/error">'
            '<env:Body>%s</env:Body></env:Envelope>' % (SOAP12_NS, body))


def soap_fault(code, subcode, reason):
    return soap_envelope(
        '<env:Fault><env:Code><env:Value>env:%s</env:Value><env:Subcode><env:Value>%s'
        '</env:Value></env:Subcode></env:Code><env:Reason><env:Text xml:lang="en">%s</env:Text>'
        '</env:Reason></env:Fault>' % (code, subcode, escape(reason)))


def onvif_date_time_xml(config, now=None):
    now = now or datetime.datetime.now(datetime.timezone.utc)
    local = now.astimezone(config.utc_offset)

    def stamp(moment):
        return ("<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d"
                "</tt:Second></tt:Time><tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month>"
                "<tt:Day>%d</tt:Day></tt:Date>" % (moment.hour, moment.minute, moment.second,
                                                   moment.year, moment.month, moment.day))

    return soap_envelope(
        "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
        "<tt:DateTimeType>Manual</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>"
        "<tt:TimeZone><tt:TZ>CST%s</tt:TZ></tt:TimeZone>"
        "<tt:UTCDateTime>%s</tt:UTCDateTime><tt:LocalDateTime>%s</tt:LocalDateTime>"
        "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>"
        % (posix_offset(config.utc_offset), stamp(now), stamp(local)))


def onvif_device_information_xml(config):
    return soap_envelope(
        "<tds:GetDeviceInformationResponse><tds:Manufacturer>HIKVISION</tds:Manufacturer>"
        "<tds:Model>%s</tds:Model><tds:FirmwareVersion>%s %s</tds:FirmwareVersion>"
        "<tds:SerialNumber>%s</tds:SerialNumber><tds:HardwareId>88</tds:HardwareId>"
        "</tds:GetDeviceInformationResponse>"
        % (escape(config.model), escape(config.firmware), escape(config.firmware_build),
           escape(config.serial)))


def index_html(config):
    return (
        "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>SIMULATED %s</title>"
        "<script>window.location.href = \"/doc/page/login.asp?_\" + (new Date()).getTime();"
        "</script></head><body><h1>Simulated Hikvision camera</h1><p>Development test fixture "
        "from tests/camera_sim, not a real device.</p><p>%s, serial %s, MAC %s, IP %s</p>"
        "</body></html>\n" % (escape(config.model), escape(config.model), escape(config.serial),
                              config.mac, config.ip))


# ---------------------------------------------------------------------------------------------
# Discovery responders


def handle_sadp_datagram(config, payload, sender, boot_time, http_port, sdk_port):
    """The ProbeMatch bytes for an SADP inquiry, None for anything else (our own answers looping
    back, mutating SADP commands, other multicast traffic)."""
    root = parse_xml_bytes(payload)
    if root is None or local_name(root.tag) != "Probe":
        return None
    types = text_of(root, "Types")
    if types.lower() not in ("inquiry", "inquiry_v32"):
        LOG.info("SADP ignored Types=%r from %s:%d (simulator answers inquiries only)",
                 types[:40], sender[0], sender[1])
        return None
    probe_uuid = text_of(root, "Uuid")
    LOG.info("SADP %s from %s:%d -> ProbeMatch (Activated=%s)", types, sender[0], sender[1],
             bool_text(config.activated))
    return sadp_probe_match(config, probe_uuid, boot_time, http_port, sdk_port).encode("utf-8")


def _scope_matches(requested, offered):
    """WS-Discovery RFC 3986 scope matching: each requested scope is a segment-wise prefix of an
    offered one."""
    def segments(scope):
        return [part for part in scope.rstrip("/").split("/")]

    for want in requested:
        if not any(segments(have)[:len(segments(want))] == segments(want) for have in offered):
            return False
    return True


def handle_ws_discovery_datagram(config, payload, sender, http_port):
    root = parse_xml_bytes(payload)
    if root is None or local_name(root.tag) != "Envelope":
        return None
    body = find_local(root, "Body")
    probe = None
    if body is not None:
        for child in body:
            if local_name(child.tag) == "Probe":
                probe = child
    if probe is None:
        return None
    types = text_of(probe, "Types")
    wanted = [item.rsplit(":", 1)[-1] for item in types.split()]
    if wanted and not any(item in ("NetworkVideoTransmitter", "Device") for item in wanted):
        LOG.info("WS-Discovery probe from %s:%d for %r ignored", sender[0], sender[1], types)
        return None
    scopes = text_of(probe, "Scopes").split()
    answer = ws_discovery_probe_match(config, text_of(root, "MessageID"), http_port)
    offered = re.search(r"<d:Scopes>(.*?)</d:Scopes>", answer).group(1).split()
    if scopes and not _scope_matches(scopes, offered):
        LOG.info("WS-Discovery probe from %s:%d with unmatched scopes ignored", *sender)
        return None
    LOG.info("WS-Discovery Probe from %s:%d -> ProbeMatch", sender[0], sender[1])
    return answer.encode("utf-8")


class UdpResponder(object):
    """A multicast-joined UDP socket that answers datagrams by unicast to the sender's address
    and port, which is what SADP and WS-Discovery devices do."""

    def __init__(self, label, port, group, interface_ip, handler, join=True):
        self.label = label
        self._handler = handler
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            try:
                self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            except OSError:
                pass
        self._sock.bind(("", port))
        self.port = self._sock.getsockname()[1]
        if join:
            membership = struct.pack("4s4s", socket.inet_aton(group),
                                     socket.inet_aton(interface_ip or "0.0.0.0"))
            try:
                self._sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
            except OSError as error:
                LOG.warning("%s: cannot join %s on %s (%s); unicast probes only", label, group,
                            interface_ip, error)
        self._sock.settimeout(POLL_SECONDS)
        self._stopping = threading.Event()
        self._thread = threading.Thread(target=self._run, name=label, daemon=True)

    def start(self):
        self._thread.start()

    def signal_stop(self):
        self._stopping.set()

    def join(self):
        self._thread.join(timeout=2)
        self._sock.close()

    def _run(self):
        while not self._stopping.is_set():
            try:
                payload, sender = self._sock.recvfrom(MAX_DATAGRAM)
            except socket.timeout:
                continue
            except OSError:
                if self._stopping.is_set():
                    return
                raise
            try:
                reply = self._handler(payload, sender)
                if reply:
                    self._sock.sendto(reply, sender)
            except Exception:  # one bad datagram must not end discovery answers
                LOG.exception("%s: failed to answer %s:%d", self.label, sender[0], sender[1])


# ---------------------------------------------------------------------------------------------
# ANPR event source shared by all alertStream connections


class EventBus(object):
    def __init__(self):
        self._subscribers = set()
        self._lock = threading.Lock()
        self.closed = threading.Event()

    def subscribe(self):
        subscriber = queue.Queue(maxsize=64)
        with self._lock:
            self._subscribers.add(subscriber)
        return subscriber

    def unsubscribe(self, subscriber):
        with self._lock:
            self._subscribers.discard(subscriber)

    def publish(self, event):
        with self._lock:
            subscribers = list(self._subscribers)
        for subscriber in subscribers:
            try:
                subscriber.put_nowait(event)
            except queue.Full:
                pass
        return len(subscribers)

    def close(self):
        self.closed.set()
        self.publish(None)


class AnprGenerator(object):
    """A plate every `interval` seconds since start (0 disables), plus one per `trigger()`
    (SIGUSR1, `sim.sh trigger-anpr`)."""

    def __init__(self, config, bus):
        self._config = config
        self._bus = bus
        self._trigger = threading.Event()
        self._stopping = threading.Event()
        self._count = 0
        self._thread = threading.Thread(target=self._run, name="anpr", daemon=True)

    def start(self):
        self._thread.start()

    def signal_stop(self):
        self._stopping.set()
        self._trigger.set()

    def join(self):
        self._thread.join(timeout=2)

    def trigger(self):
        self._trigger.set()

    def emit(self):
        self._count += 1
        event = AnprEvent(self._config.anpr_plate, self._count,
                          datetime.datetime.now(datetime.timezone.utc), str(uuid.uuid4()))
        listeners = self._bus.publish(event)
        LOG.info("ANPR event plate=%s uuid=%s country=%d alertStream listeners=%d",
                 event.plate, event.uuid, self._config.anpr_country, listeners)
        return event

    def _run(self):
        interval = self._config.anpr_interval
        next_due = time.monotonic() + interval if interval > 0 else None
        while not self._stopping.is_set():
            timeout = None if next_due is None else max(0.0, next_due - time.monotonic())
            triggered = self._trigger.wait(timeout)
            if self._stopping.is_set():
                return
            if triggered:
                self._trigger.clear()
                self.emit()
            elif next_due is not None:
                self.emit()
                next_due += interval
                if next_due < time.monotonic():
                    next_due = time.monotonic() + interval


# ---------------------------------------------------------------------------------------------
# HTTP: ISAPI, ONVIF device service, web page


class _ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 64


class CameraHTTPHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "webserver"
    timeout = 60

    def version_string(self):
        return "webserver"

    def log_message(self, fmt, *args):
        pass

    def log_error(self, fmt, *args):
        LOG.info("HTTP %s error: %s", self.client_address[0], fmt % args)

    def do_GET(self):
        self._dispatch()

    def do_POST(self):
        self._dispatch()

    def do_PUT(self):
        self._dispatch()

    def do_DELETE(self):
        self._dispatch()

    # -- plumbing ------------------------------------------------------------------------------

    @property
    def camera(self):
        return self.server.camera

    def _read_body(self):
        length = self.headers.get("Content-Length")
        if not length:
            return b""
        try:
            size = int(length)
        except ValueError:
            return b""
        if size > MAX_REQUEST_BODY:
            self.close_connection = True
            return b""
        return self.rfile.read(size)

    def _log_request(self, status, note):
        LOG.info('HTTP %s "%s %s" %s %s', self.client_address[0], self.command, self.path,
                 status, note)

    def _send(self, status, body, content_type, note, extra_headers=()):
        """Logs the request, then answers it: logging first keeps the log ordered before the
        client sees the response, which tests rely on."""
        self._log_request(status, note)
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Date", self.date_time_string())
        for name, value in extra_headers:
            self.send_header(name, value)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_xml(self, status, body, note, extra_headers=()):
        self._send(status, body, 'application/xml; charset="UTF-8"', note, extra_headers)

    def _dispatch(self):
        body = self._read_body()
        path = urlsplit(self.path).path
        camera = self.camera
        if not camera.config.activated:
            self._send_xml(401, user_check_xml(401, 0, 0, activated=False), "not-activated",
                           [("WWW-Authenticate", camera.isapi_auth.challenge())])
            return
        lowered = path.lower()
        if lowered.startswith("/isapi/"):
            self._isapi(path, lowered)
        elif lowered.startswith("/onvif/"):
            self._onvif(lowered, body)
        elif self.command == "GET" and lowered in ("/", "/index.asp", "/doc/page/login.asp"):
            self._send(200, index_html(camera.config), "text/html; charset=utf-8", "page")
        else:
            self._send(404, "<html><body>404 Not Found</body></html>\n", "text/html",
                       "not-found")

    # -- ISAPI ---------------------------------------------------------------------------------

    def _isapi(self, path, lowered):
        camera = self.camera
        ip = self.client_address[0]
        guard = camera.guard
        locked = guard.locked_remaining(ip)
        if locked > 0:
            LOG.warning("LOCKED reject ip=%s %s %s (%ds left)", ip, self.command, path,
                        int(locked))
            self._send_xml(401, user_check_xml(401, locked, 0), "locked",
                           [("WWW-Authenticate", camera.isapi_auth.challenge())])
            return
        result = camera.isapi_auth.verify(self.command, self.path,
                                          self.headers.get("Authorization"))
        if result.status != DigestResult.OK:
            retries = guard.attempts - guard.failures(ip)
            note = "auth=%s" % result.status
            locked_seconds = 0
            if result.status == DigestResult.FAILED:
                count, locked_now = guard.record_failure(ip, "isapi")
                retries = guard.attempts - count
                LOG.warning("AUTH_FAIL ip=%s source=isapi user=%r reason=%s failures=%d/%d", ip,
                            result.username, result.reason, count, guard.attempts)
                note += " user=%r" % result.username
                locked_seconds = guard.duration if locked_now else 0
            stale = result.status == DigestResult.STALE
            self._send_xml(401, user_check_xml(401, locked_seconds, retries), note,
                           [("WWW-Authenticate", camera.isapi_auth.challenge(stale=stale))])
            return
        note = "auth=ok user=%r" % result.username
        if self.command != "GET":
            LOG.warning("WRITE_REJECTED ip=%s %s %s (the simulator is read-only)", ip,
                        self.command, path)
            self._send_xml(403, response_status_xml(path, 4, "Invalid Operation", "notSupport"),
                           note)
            return
        if lowered == "/isapi/event/notification/alertstream":
            self._alert_stream(note)
            return
        document = self._isapi_document(lowered)
        if document is None:
            self._send_xml(404, response_status_xml(path, 4, "Invalid Operation", "notSupport"),
                           note)
        else:
            self._send_xml(200, document, note)

    def _isapi_document(self, lowered):
        config = self.camera.config
        if lowered == "/isapi/system/deviceinfo":
            return device_info_xml(config)
        if lowered == "/isapi/system/time":
            return time_xml(config)
        if lowered == "/isapi/system/network/interfaces":
            return network_interfaces_xml(config)
        if lowered == "/isapi/system/network/integrate":
            return integrate_xml(config)
        if lowered == "/isapi/security/usercheck":
            return user_check_xml(200, 0, self.camera.guard.attempts)
        if lowered == "/isapi/traffic/capabilities":
            return traffic_capabilities_xml(config)
        if lowered == "/isapi/streaming/channels":
            return streaming_channel_list_xml(config)
        match = re.match(r"^/isapi/streaming/channels/(\d+)$", lowered)
        if match:
            channel = config.channel(int(match.group(1)))
            return streaming_channel_xml(config, channel) if channel else None
        return None

    def _alert_stream(self, note):
        """multipart/mixed over chunked encoding until the client leaves or the camera stops:
        a heartbeat every `heartbeat_interval` seconds and every ANPR event as it happens."""
        camera = self.camera
        ip = self.client_address[0]
        self._log_request(200, note + " stream-open")
        self.send_response(200)
        self.send_header("Date", self.date_time_string())
        self.send_header("MIME-Version", "1.0")
        self.send_header("Connection", "close")
        self.send_header("Content-Type", "multipart/mixed; boundary=%s" % ALERT_BOUNDARY)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        self.close_connection = True
        subscriber = camera.bus.subscribe()
        xml_part = [("Content-Type", 'application/xml; charset="UTF-8"')]
        parts_sent = 0
        try:
            self._write_chunk(multipart_part(xml_part, heartbeat_alert_xml(camera.config,
                                                                           camera.http_port)))
            next_heartbeat = time.monotonic() + camera.config.heartbeat_interval
            while not camera.bus.closed.is_set():
                try:
                    event = subscriber.get(timeout=max(0.05, next_heartbeat - time.monotonic()))
                except queue.Empty:
                    self._write_chunk(multipart_part(xml_part, heartbeat_alert_xml(
                        camera.config, camera.http_port)))
                    next_heartbeat += camera.config.heartbeat_interval
                    continue
                if event is None:
                    break
                self._write_chunk(camera.anpr_parts(event))
                parts_sent += 1
            self._write_chunk(("--%s--\r\n" % ALERT_BOUNDARY).encode("ascii"))
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, socket.timeout, OSError):
            pass
        finally:
            camera.bus.unsubscribe(subscriber)
            LOG.info("HTTP %s alertStream closed after %d ANPR event(s)", ip, parts_sent)

    def _write_chunk(self, data):
        self.wfile.write(("%X\r\n" % len(data)).encode("ascii") + data + b"\r\n")
        self.wfile.flush()

    # -- ONVIF ---------------------------------------------------------------------------------

    def _onvif(self, lowered, body):
        camera = self.camera
        soap_type = "application/soap+xml; charset=utf-8"
        if not camera.config.onvif:
            self._send(404, "<html><body>404 Not Found</body></html>\n", "text/html",
                       "onvif-disabled")
            return
        if lowered != "/onvif/device_service":
            self._send(400, soap_fault("Sender", "ter:ActionNotSupported",
                                       "The simulator implements the device service only"),
                       soap_type, "onvif-unsupported-service")
            return
        if self.command != "POST":
            self._send(405, soap_fault("Sender", "ter:ActionNotSupported", "POST expected"),
                       soap_type, "onvif-method", [("Allow", "POST")])
            return
        envelope = parse_xml_bytes(body)
        soap_body = find_local(envelope, "Body") if envelope is not None else None
        if soap_body is None or len(soap_body) == 0:
            self._send(400, soap_fault("Sender", "ter:WellFormed", "Malformed SOAP request"),
                       soap_type, "onvif-malformed")
            return
        action = local_name(soap_body[0].tag)
        if action == "GetSystemDateAndTime":
            self._send(200, onvif_date_time_xml(camera.config), soap_type, "onvif=%s" % action)
            return
        if action != "GetDeviceInformation":
            self._send(500, soap_fault("Receiver", "ter:ActionNotSupported",
                                       "%s is not simulated" % action), soap_type,
                       "onvif=%s unsupported" % action)
            return
        ip = self.client_address[0]
        locked = camera.guard.locked_remaining(ip)
        if locked > 0:
            LOG.warning("LOCKED reject ip=%s ONVIF %s (%ds left)", ip, action, int(locked))
            self._send(401, soap_fault("Sender", "ter:NotAuthorized",
                                       "Sender not authorized (locked)"), soap_type,
                       "onvif=%s locked" % action)
            return
        result = verify_username_token(envelope, camera.config.onvif_username,
                                       camera.config.onvif_password,
                                       camera.config.onvif_clock_skew)
        if result.status == UsernameTokenResult.FAILED:
            count, _ = camera.guard.record_failure(ip, "onvif")
            LOG.warning("AUTH_FAIL ip=%s source=onvif user=%r reason=%s failures=%d/%d", ip,
                        result.username, result.reason, count, camera.guard.attempts)
        if result.status != UsernameTokenResult.OK:
            self._send(401, soap_fault("Sender", "ter:NotAuthorized",
                                       "The action requested requires authorization and the "
                                       "sender is not authorized"), soap_type,
                       "onvif=%s auth=%s" % (action, result.status))
            return
        self._send(200, onvif_device_information_xml(camera.config), soap_type,
                   "onvif=%s auth=ok user=%r" % (action, result.username))


# ---------------------------------------------------------------------------------------------
# SDK port and the MediaMTX log watcher


class SdkPortListener(object):
    """Accepts TCP on the SDK ("server") port so port probes see what a camera shows; it does
    not speak the proprietary SDK protocol and closes after the client goes quiet."""

    def __init__(self, bind, port):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((bind, port))
        self._sock.listen(16)
        self._sock.settimeout(POLL_SECONDS)
        self.port = self._sock.getsockname()[1]
        self._slots = threading.BoundedSemaphore(32)
        self._stopping = threading.Event()
        self._thread = threading.Thread(target=self._run, name="sdk", daemon=True)

    def start(self):
        self._thread.start()

    def signal_stop(self):
        self._stopping.set()

    def join(self):
        self._thread.join(timeout=2)
        self._sock.close()

    def _run(self):
        while not self._stopping.is_set():
            try:
                connection, peer = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            if not self._slots.acquire(blocking=False):
                connection.close()
                continue
            threading.Thread(target=self._serve, args=(connection, peer), daemon=True).start()

    def _serve(self, connection, peer):
        received = 0
        try:
            connection.settimeout(10)
            while True:
                data = connection.recv(4096)
                if not data:
                    break
                received += len(data)
        except (socket.timeout, OSError):
            pass
        finally:
            connection.close()
            self._slots.release()
            LOG.info("SDK port connection from %s closed (%d bytes; SDK protocol not simulated)",
                     peer[0], received)


RTSP_AUTH_FAILURE = re.compile(r"\[conn ([0-9.]+):\d+\] failed to authenticate")


class RtspLogWatcher(object):
    """Counts MediaMTX's RTSP authentication failures (read from its log file) into the shared
    login budget, so a wrong RTSP password moves the simulated camera towards its lock exactly
    like an ISAPI one. MediaMTX keeps serving RTSP itself; only ISAPI/ONVIF enforce the lock."""

    def __init__(self, path, guard, poll_interval=0.5):
        self._path = path
        self._guard = guard
        self._poll = poll_interval
        self._offset = os.path.getsize(path) if os.path.exists(path) else 0
        self._partial = b""
        self._stopping = threading.Event()
        self._thread = threading.Thread(target=self._run, name="rtsp-log", daemon=True)

    def start(self):
        self._thread.start()

    def signal_stop(self):
        self._stopping.set()

    def join(self):
        self._thread.join(timeout=2)

    def poll_once(self):
        try:
            size = os.path.getsize(self._path)
        except OSError:
            return
        if size < self._offset:
            self._offset = 0
            self._partial = b""
        if size == self._offset:
            return
        with open(self._path, "rb") as handle:
            handle.seek(self._offset)
            data = handle.read(size - self._offset)
        self._offset += len(data)
        lines = (self._partial + data).split(b"\n")
        self._partial = lines.pop()
        for line in lines:
            match = RTSP_AUTH_FAILURE.search(line.decode("utf-8", "replace"))
            if match:
                ip = match.group(1)
                count, _ = self._guard.record_failure(ip, "rtsp")
                LOG.warning("AUTH_FAIL ip=%s source=rtsp failures=%d/%d", ip, count,
                            self._guard.attempts)

    def _run(self):
        while not self._stopping.wait(self._poll):
            self.poll_once()


# ---------------------------------------------------------------------------------------------
# The camera


class FakeCamera(object):
    def __init__(self, config):
        self.config = config
        self.boot_time = datetime.datetime.now(config.utc_offset).strftime("%Y-%m-%d %H:%M:%S")
        self.guard = LoginGuard(config.lockout_attempts, config.lockout_window,
                                config.lockout_duration)
        self.isapi_auth = DigestAuthenticator(config.realm, {config.username: config.password},
                                              config.nonce_lifetime)
        self.bus = EventBus()
        self.anpr = AnprGenerator(config, self.bus)
        self._pictures = []
        for path in config.anpr_pictures:
            with open(path, "rb") as handle:
                self._pictures.append((os.path.basename(path), handle.read()))
        self._http = None
        self._http_thread = None
        self._sdk = None
        self._sadp = None
        self._wsd = None
        self._rtsp_log = None
        self.http_port = config.http_port
        self.sdk_port = config.sdk_port

    def anpr_parts(self, event):
        """The XML part and one JPEG part per configured picture, as a camera sends them."""
        names = [name for name, _ in self._pictures]
        data = multipart_part(
            [("Content-Type", 'application/xml; charset="UTF-8"'),
             ("Content-Disposition", 'form-data; name="anpr.xml"; filename="anpr.xml"')],
            anpr_alert_xml(self.config, self.http_port, event, names))
        for index, (name, picture) in enumerate(self._pictures):
            data += multipart_part(
                [("Content-Disposition", 'form-data; name="%s"; filename="%s"' % (name, name)),
                 ("Content-Type", "image/jpeg"), ("Content-ID", "image_%d" % (index + 1))],
                picture)
        return data

    def start(self):
        config = self.config
        self._http = _ThreadingHTTPServer((config.bind, config.http_port), CameraHTTPHandler)
        self._http.camera = self
        self.http_port = self._http.server_address[1]
        self._sdk = SdkPortListener(config.bind, config.sdk_port)
        self.sdk_port = self._sdk.port
        self._sadp = UdpResponder(
            "sadp", config.sadp_port, SADP_GROUP, config.ip,
            lambda payload, sender: handle_sadp_datagram(config, payload, sender, self.boot_time,
                                                         self.http_port, self.sdk_port),
            join=config.multicast)
        if config.onvif:
            self._wsd = UdpResponder(
                "ws-discovery", config.wsd_port, WSD_GROUP, config.ip,
                lambda payload, sender: handle_ws_discovery_datagram(config, payload, sender,
                                                                     self.http_port),
                join=config.multicast)
        if config.rtsp_log:
            self._rtsp_log = RtspLogWatcher(config.rtsp_log, self.guard)
        self._http_thread = threading.Thread(target=self._http.serve_forever,
                                             kwargs={"poll_interval": POLL_SECONDS}, name="http",
                                             daemon=True)
        self._http_thread.start()
        for service in (self._sdk, self._sadp, self._wsd, self._rtsp_log, self.anpr):
            if service is not None:
                service.start()

    @property
    def sadp_port(self):
        return self._sadp.port if self._sadp else None

    @property
    def wsd_port(self):
        return self._wsd.port if self._wsd else None

    def stop(self):
        """Ends alertStreams first, then stops every service in parallel."""
        self.bus.close()
        services = [service for service in (self.anpr, self._rtsp_log, self._wsd, self._sadp,
                                            self._sdk) if service is not None]
        for service in services:
            service.signal_stop()
        if self._http is not None:
            self._http.shutdown()
            self._http.server_close()
        for service in services:
            service.join()

    def describe(self):
        config = self.config
        return ("SIMULATED Hikvision %s serial=%s mac=%s ip=%s/%s gw=%s http=%d rtsp=%d sdk=%d "
                "activated=%s onvif=%s realm=%r user=%r channels=%s anpr=%s every %ss"
                % (config.model, config.serial, config.mac, config.ip, config.netmask,
                   config.gateway, self.http_port, config.rtsp_port, self.sdk_port,
                   bool_text(config.activated), "on" if config.onvif else "off", config.realm,
                   config.username,
                   ",".join("%d:%s:%dx%d@%d" % (c.channel_id, c.codec, c.width, c.height,
                                                c.max_frame_rate) for c in config.channels),
                   config.anpr_plate, config.anpr_interval))


# ---------------------------------------------------------------------------------------------
# Command line


def _env(name, default):
    return os.environ.get("FAKE_HIK_" + name.upper(), default)


def build_parser():
    parser = argparse.ArgumentParser(
        description="SIMULATED Hikvision camera services (development tests only). Every "
                    "option can also be set as FAKE_HIK_<OPTION> in the environment, e.g. "
                    "FAKE_HIK_PASSWORD.")
    add = parser.add_argument
    add("--name", default=_env("name", socket.gethostname()), help="label used in the log")
    add("--model", default=_env("model", "DS-TCG406-E"))
    add("--serial", default=_env("serial", ""), help="default: derived from model, IP and MAC")
    add("--interface", default=_env("interface", "eth0"),
        help="where --mac/--ip/--netmask/--gateway defaults are read from")
    add("--mac", default=_env("mac", ""), help="default: /sys/class/net/<interface>/address")
    add("--ip", default=_env("ip", ""), help="default: the interface's IPv4 address")
    add("--netmask", default=_env("netmask", ""))
    add("--gateway", default=_env("gateway", ""))
    add("--bind", default=_env("bind", "0.0.0.0"), help="address of the TCP listeners")
    add("--http-port", type=int, default=int(_env("http_port", 80)))
    add("--rtsp-port", type=int, default=int(_env("rtsp_port", 554)),
        help="reported only: RTSP is served by MediaMTX")
    add("--sdk-port", type=int, default=int(_env("sdk_port", 8000)))
    add("--sadp-port", type=int, default=int(_env("sadp_port", SADP_PORT)))
    add("--wsd-port", type=int, default=int(_env("wsd_port", WSD_PORT)))
    add("--no-multicast", action="store_true", default=parse_bool(_env("no_multicast", "false")),
        help="do not join 239.255.255.250 (unit tests send unicast probes)")
    add("--activated", type=parse_bool, default=parse_bool(_env("activated", "true")))
    add("--onvif", type=parse_bool, default=parse_bool(_env("onvif", "off")))
    add("--username", default=_env("username", "admin"))
    add("--password", default=_env("password", ""),
        help="prefer FAKE_HIK_PASSWORD: arguments are visible in the process list")
    add("--onvif-username", default=_env("onvif_username", ""),
        help="separate ONVIF account (default: --username)")
    add("--onvif-password", default=_env("onvif_password", ""))
    add("--realm", default=_env("realm", ""), help='default "IP Camera(SIMnn)"')
    add("--device-name", default=_env("device_name", ""))
    add("--firmware", default=_env("firmware", "V5.7.10"))
    add("--channel", action="append", type=Channel.parse, default=None,
        help="ID,CODEC,WIDTHxHEIGHT,CENTI_FPS[,GOV], repeatable (e.g. 101,H.264,608x1080,3000)")
    add("--anpr-plate", default=_env("anpr_plate", "152JTA02"))
    add("--anpr-country", type=int, default=int(_env("anpr_country", 30)),
        help="Hikvision country code (30 = Kazakhstan)")
    add("--anpr-interval", type=float, default=float(_env("anpr_interval", 20)),
        help="seconds between ANPR events, 0 = only on SIGUSR1")
    add("--anpr-picture", action="append", default=None,
        help="JPEG attached to every ANPR event under its file name (licensePlatePicture.jpg, "
             "vehiclePicture.jpg, detectionPicture.jpg), repeatable")
    add("--anpr-plate-rect", default=_env("anpr_plate_rect", "338,338,117,44"),
        help="X,Y,W,H of the plate in the vehicle/detection picture")
    add("--heartbeat-interval", type=float, default=float(_env("heartbeat_interval", 5)))
    add("--lockout-attempts", type=int, default=int(_env("lockout_attempts", 5)))
    add("--lockout-window", type=float, default=float(_env("lockout_window", 1800)))
    add("--lockout-duration", type=float, default=float(_env("lockout_duration", 1800)))
    add("--nonce-lifetime", type=float, default=float(_env("nonce_lifetime", 300)))
    add("--rtsp-log", default=_env("rtsp_log", ""),
        help="MediaMTX log file whose RTSP auth failures count towards the lock")
    add("--utc-offset", type=parse_utc_offset, default=parse_utc_offset(_env("utc_offset",
                                                                              "+05:00")))
    add("--log-level", default=_env("log_level", "INFO"))
    return parser


def config_from_args(args):
    if args.channel is None:
        env_channels = _env("channels", "")
        args.channel = [Channel.parse(item) for item in env_channels.split(";") if item.strip()]
    if args.anpr_picture is None:
        args.anpr_picture = [item for item in _env("anpr_pictures", "").split(";") if item]
    rect = [part.strip() for part in args.anpr_plate_rect.split(",")]
    if len(rect) != 4 or not all(part.isdigit() for part in rect):
        raise SystemExit("fake_hikvision: --anpr-plate-rect must be X,Y,W,H")
    mac = args.mac or interface_mac(args.interface) or normalize_mac("%012x" % uuid.getnode())
    ip = args.ip or interface_ipv4(args.interface) or "127.0.0.1"
    if not args.password:
        raise SystemExit("fake_hikvision: set --password or FAKE_HIK_PASSWORD")
    return CameraConfig(
        name=args.name, model=args.model, serial=args.serial, mac=mac, ip=ip,
        netmask=args.netmask or interface_netmask(args.interface) or "255.255.255.0",
        gateway=args.gateway or default_gateway(args.interface) or "0.0.0.0",
        bind=args.bind, http_port=args.http_port, rtsp_port=args.rtsp_port,
        sdk_port=args.sdk_port, sadp_port=args.sadp_port, wsd_port=args.wsd_port,
        multicast=not args.no_multicast, activated=args.activated, onvif=args.onvif,
        username=args.username, password=args.password, onvif_username=args.onvif_username,
        onvif_password=args.onvif_password, realm=args.realm, device_name=args.device_name,
        firmware=args.firmware, channels=args.channel, anpr_plate=args.anpr_plate,
        anpr_country=args.anpr_country, anpr_interval=args.anpr_interval,
        anpr_pictures=args.anpr_picture, anpr_plate_rect=tuple(int(part) for part in rect),
        heartbeat_interval=args.heartbeat_interval,
        lockout_attempts=args.lockout_attempts, lockout_window=args.lockout_window,
        lockout_duration=args.lockout_duration, nonce_lifetime=args.nonce_lifetime,
        rtsp_log=args.rtsp_log, utc_offset=args.utc_offset)


def setup_logging(name, level):
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(logging.Formatter(
        "%(asctime)s.%(msecs)03d fake_hikvision[" + name + "] %(message)s",
        datefmt="%Y-%m-%dT%H:%M:%S"))
    LOG.handlers[:] = [handler]
    LOG.setLevel(level.upper())
    LOG.propagate = False


def main(argv=None):
    args = build_parser().parse_args(argv)
    setup_logging(args.name, args.log_level)
    camera = FakeCamera(config_from_args(args))
    stopping = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopping.set())
    signal.signal(signal.SIGINT, lambda *_: stopping.set())
    if hasattr(signal, "SIGUSR1"):
        signal.signal(signal.SIGUSR1, lambda *_: camera.anpr.trigger())
    camera.start()
    LOG.info("started: %s", camera.describe())
    while not stopping.wait(1.0):
        pass
    LOG.info("stopping")
    camera.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
