#!/usr/bin/env python3
"""Unit tests for the camera simulator (fake_hikvision.py, sadp_probe.py).

Standard library only, 127.0.0.1 only, no root: every service binds an ephemeral port and the
discovery responders are probed by unicast. Run from anywhere:

    python3 tests/camera_sim/test_fake_hikvision.py -v
"""

import base64
import datetime
import hashlib
import http.client
import io
import logging
import os
import re
import socket
import sys
import tempfile
import threading
import time
import unittest
import urllib.request
import xml.etree.ElementTree as ET
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fake_hikvision as fh  # noqa: E402
import sadp_probe  # noqa: E402

# The simulator logs to its own handlers only; keep stray warnings out of the test output.
fh.LOG.propagate = False
fh.LOG.addHandler(logging.NullHandler())

PASSWORD = "Unit-test_pw9"
ONVIF_PASSWORD = "Onvif-unit_pw7"
HIK = "{%s}" % fh.HIK_NS


class FakeClock(object):
    def __init__(self, start=1000.0):
        self.now = start

    def __call__(self):
        return self.now


class LogCollector(logging.Handler):
    """Captures the simulator's log lines (it does not propagate to the root logger)."""

    def __init__(self):
        super(LogCollector, self).__init__()
        self.lines = []
        self._lock = threading.Lock()

    def emit(self, record):
        with self._lock:
            self.lines.append(record.getMessage())

    def text(self):
        with self._lock:
            return "\n".join(self.lines)


def digest_header(method, uri, challenge, username, password, nc="00000001",
                  cnonce="0a4f113b"):
    params = fh.parse_auth_params(challenge.partition(" ")[2])
    ha1 = fh.md5_hex("%s:%s:%s" % (username, params["realm"], password))
    ha2 = fh.md5_hex("%s:%s" % (method, uri))
    response = fh.md5_hex("%s:%s:%s:%s:auth:%s" % (ha1, params["nonce"], nc, cnonce, ha2))
    return ('Digest username="%s", realm="%s", nonce="%s", uri="%s", algorithm=MD5, '
            'response="%s", qop=auth, nc=%s, cnonce="%s"'
            % (username, params["realm"], params["nonce"], uri, response, nc, cnonce))


def make_config(**overrides):
    values = dict(name="unit", ip="192.168.77.21", mac="bc:ad:28:77:00:21",
                  netmask="255.255.255.0", gateway="192.168.77.1", bind="127.0.0.1",
                  http_port=0, sdk_port=0, sadp_port=0, wsd_port=0, multicast=False,
                  username="admin", password=PASSWORD, anpr_interval=0,
                  heartbeat_interval=5.0,
                  channels=[fh.Channel(101, "H.264", 608, 1080, 3000, 160),
                            fh.Channel(102, "H.265", 304, 540, 3000, 60)])
    values.update(overrides)
    return fh.CameraConfig(**values)


# ---------------------------------------------------------------------------------------------


class HelperTests(unittest.TestCase):
    def test_normalize_mac(self):
        self.assertEqual(fh.normalize_mac("BC-AD-28-77-00-21"), "bc:ad:28:77:00:21")
        self.assertEqual(fh.normalize_mac("bcad28770021", "-"), "bc-ad-28-77-00-21")
        with self.assertRaises(ValueError):
            fh.normalize_mac("bc:ad:28")

    def test_utc_offset_and_iso_time(self):
        tz = fh.parse_utc_offset("+05:00")
        moment = datetime.datetime(2026, 10, 8, 20, 1, 2, tzinfo=datetime.timezone.utc)
        self.assertEqual(fh.iso_with_offset(moment, tz), "2026-10-09T01:01:02+05:00")
        self.assertEqual(fh.iso_with_offset(moment, fh.parse_utc_offset("-0330")),
                         "2026-10-08T16:31:02-03:30")
        self.assertEqual(fh.posix_offset(tz), "-5:00:00")
        with self.assertRaises(Exception):
            fh.parse_utc_offset("5")

    def test_parse_soap_datetime(self):
        utc = datetime.timezone.utc
        self.assertEqual(fh.parse_soap_datetime("2026-10-09T01:02:03Z"),
                         datetime.datetime(2026, 10, 9, 1, 2, 3, tzinfo=utc))
        self.assertEqual(fh.parse_soap_datetime("2026-10-09T06:02:03.250+05:00"),
                         datetime.datetime(2026, 10, 9, 1, 2, 3, 250000, tzinfo=utc))
        self.assertIsNone(fh.parse_soap_datetime("yesterday"))
        self.assertIsNone(fh.parse_soap_datetime("2026-13-40T01:02:03Z"))

    def test_channel_parse(self):
        channel = fh.Channel.parse("101,h265,608x1080,3000,60")
        self.assertEqual((channel.channel_id, channel.codec, channel.width, channel.height,
                          channel.max_frame_rate, channel.gov_length),
                         (101, "H.265", 608, 1080, 3000, 60))
        self.assertEqual(fh.Channel.parse("102,H.264,304x540,2500").gov_length, 50)
        for bad in ("101,H.264,608x1080", "x,H.264,608x1080,3000", "101,VP8,608x1080,3000",
                    "101,H.264,608-1080,3000"):
            with self.assertRaises(Exception):
                fh.Channel.parse(bad)

    def test_config_defaults_are_labelled_simulation(self):
        config = make_config()
        self.assertEqual(config.realm, "IP Camera(SIM21)")
        self.assertTrue(config.serial.startswith("DS-TCG406-E"))
        self.assertIn("SIM21", config.serial)
        self.assertEqual(config.device_name, "SIM unit")
        self.assertTrue(config.onvif_endpoint_uuid.endswith("bcad28770021"))
        self.assertEqual(len(config.onvif_endpoint_uuid), 36)
        self.assertEqual(config.onvif_username, "admin")
        with self.assertRaises(TypeError):
            fh.CameraConfig(no_such_setting=1)

    def test_parse_auth_params_handles_quotes_and_commas(self):
        params = fh.parse_auth_params('username="a,b", realm="IP Camera(SIM21)", nc=00000001, '
                                      'uri="/x?y=1,2", qop=auth')
        self.assertEqual(params["username"], "a,b")
        self.assertEqual(params["realm"], "IP Camera(SIM21)")
        self.assertEqual(params["nc"], "00000001")
        self.assertEqual(params["uri"], "/x?y=1,2")


class LoginGuardTests(unittest.TestCase):
    def test_locks_after_attempts_within_window(self):
        clock = FakeClock()
        guard = fh.LoginGuard(5, 1800, 1800, clock)
        for expected in range(1, 5):
            self.assertEqual(guard.record_failure("10.0.0.7", "isapi"), (expected, False))
        self.assertEqual(guard.locked_remaining("10.0.0.7"), 0.0)
        self.assertEqual(guard.record_failure("10.0.0.7", "rtsp"), (5, True))
        self.assertAlmostEqual(guard.locked_remaining("10.0.0.7"), 1800.0)
        self.assertEqual(guard.locked_remaining("10.0.0.8"), 0.0)
        # Failures while locked neither extend nor re-lock.
        self.assertEqual(guard.record_failure("10.0.0.7", "isapi"), (5, False))
        clock.now += 1799
        self.assertAlmostEqual(guard.locked_remaining("10.0.0.7"), 1.0)
        clock.now += 2
        self.assertEqual(guard.locked_remaining("10.0.0.7"), 0.0)
        self.assertEqual(guard.failures("10.0.0.7"), 0)

    def test_old_failures_leave_the_window(self):
        clock = FakeClock()
        guard = fh.LoginGuard(5, 1800, 1800, clock)
        for _ in range(4):
            guard.record_failure("10.0.0.7", "isapi")
        clock.now += 1801
        self.assertEqual(guard.failures("10.0.0.7"), 0)
        self.assertEqual(guard.record_failure("10.0.0.7", "isapi"), (1, False))


class DigestTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.auth = fh.DigestAuthenticator("IP Camera(SIM21)", {"admin": PASSWORD}, 300,
                                           self.clock)

    def test_challenge_format(self):
        challenge = self.auth.challenge()
        self.assertTrue(challenge.startswith('Digest qop="auth", realm="IP Camera(SIM21)", '
                                             'nonce="'))
        self.assertIn('stale="FALSE"', challenge)
        self.assertIn('stale="TRUE"', self.auth.challenge(stale=True))

    def test_valid_and_invalid_responses(self):
        uri = "/ISAPI/System/deviceInfo"
        good = digest_header("GET", uri, self.auth.challenge(), "admin", PASSWORD)
        self.assertEqual(self.auth.verify("GET", uri, good).status, fh.DigestResult.OK)
        self.assertEqual(self.auth.verify("GET", uri, None).status,
                         fh.DigestResult.NO_CREDENTIALS)
        wrong = digest_header("GET", uri, self.auth.challenge(), "admin", "nope")
        result = self.auth.verify("GET", uri, wrong)
        self.assertEqual((result.status, result.reason), (fh.DigestResult.FAILED,
                                                           "wrong-response"))
        other_user = digest_header("GET", uri, self.auth.challenge(), "root", PASSWORD)
        self.assertIn("unknown-user", self.auth.verify("GET", uri, other_user).reason)
        other_uri = digest_header("GET", "/ISAPI/x", self.auth.challenge(), "admin", PASSWORD)
        self.assertIn("uri", self.auth.verify("GET", uri, other_uri).reason)
        no_qop = re.sub(r", qop=auth", "", good)
        self.assertIn("qop", self.auth.verify("GET", uri, no_qop).reason)
        basic = "Basic " + base64.b64encode(b"admin:" + PASSWORD.encode()).decode()
        self.assertEqual(self.auth.verify("GET", uri, basic).status, fh.DigestResult.FAILED)
        sha = good.replace("algorithm=MD5", "algorithm=SHA-256")
        self.assertIn("algorithm", self.auth.verify("GET", uri, sha).reason)

    def test_unknown_or_expired_nonce_is_stale_not_failed(self):
        uri = "/ISAPI/System/deviceInfo"
        forged = digest_header("GET", uri, 'Digest realm="IP Camera(SIM21)", nonce="abc"',
                               "admin", PASSWORD)
        self.assertEqual(self.auth.verify("GET", uri, forged).status, fh.DigestResult.STALE)
        header = digest_header("GET", uri, self.auth.challenge(), "admin", PASSWORD)
        self.clock.now += 301
        self.assertEqual(self.auth.verify("GET", uri, header).status, fh.DigestResult.STALE)


def soap(inner, header=""):
    return ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="%s" '
            'xmlns:tds="http://www.onvif.org/ver10/device/wsdl"><s:Header>%s</s:Header>'
            '<s:Body>%s</s:Body></s:Envelope>' % (fh.SOAP12_NS, header, inner))


class UsernameTokenTests(unittest.TestCase):
    def envelope(self, username, password, created, password_type="#PasswordDigest",
                 nonce=b"0123456789abcdef"):
        if password_type == "#PasswordDigest":
            value = fh.onvif_password_digest(nonce, created, password)
        else:
            value = password
        header = ('<Security xmlns="%s"><UsernameToken><Username>%s</Username>'
                  '<Password Type="%s%s">%s</Password><Nonce>%s</Nonce>'
                  '<Created xmlns="%s">%s</Created></UsernameToken></Security>'
                  % (sadp_probe.WSSE_NS, username, sadp_probe.TOKEN_PROFILE, password_type,
                     value, base64.b64encode(nonce).decode(), sadp_probe.WSU_NS, created))
        return ET.fromstring(soap("<tds:GetDeviceInformation/>", header).encode())

    def test_digest_rules(self):
        now = datetime.datetime(2026, 10, 9, 1, 0, 0, tzinfo=datetime.timezone.utc)
        created = "2026-10-09T01:00:30Z"
        ok = fh.verify_username_token(self.envelope("onvif", "pw", created), "onvif", "pw",
                                      300, now)
        self.assertEqual(ok.status, fh.UsernameTokenResult.OK)
        cases = [
            (self.envelope("onvif", "bad", created), "wrong-digest"),
            (self.envelope("other", "pw", created), "unknown-user"),
            (self.envelope("onvif", "pw", created, "#PasswordText"), "not-digest"),
            (self.envelope("onvif", "pw", "2026-10-09T01:10:00Z"), "clock-skew"),
            (self.envelope("onvif", "pw", "not a time"), "created"),
        ]
        for envelope, reason in cases:
            result = fh.verify_username_token(envelope, "onvif", "pw", 300, now)
            self.assertEqual((result.status, result.reason),
                             (fh.UsernameTokenResult.FAILED, reason))
        bare = ET.fromstring(soap("<tds:GetDeviceInformation/>").encode())
        self.assertEqual(fh.verify_username_token(bare, "onvif", "pw", 300, now).status,
                         fh.UsernameTokenResult.NO_CREDENTIALS)

    def test_digest_matches_reference_formula(self):
        nonce, created = b"\x01\x02\x03", "2026-10-09T01:00:00Z"
        expected = base64.b64encode(hashlib.sha1(nonce + created.encode() + b"secret")
                                    .digest()).decode()
        self.assertEqual(fh.onvif_password_digest(nonce, created, "secret"), expected)


class DocumentTests(unittest.TestCase):
    def setUp(self):
        self.config = make_config()

    def test_sadp_probe_match_fields(self):
        reply = fh.sadp_probe_match(self.config, "ABC-123", "2026-10-09 01:02:03", 80, 8000)
        root = ET.fromstring(reply.encode())
        self.assertEqual(root.tag, "ProbeMatch")
        # research/hikvision.md section 1, verbatim.
        self.assertEqual([child.tag for child in root], [
            "Uuid", "Types", "DeviceType", "DeviceDescription", "DeviceSN", "MAC",
            "IPv4Address", "IPv4SubnetMask", "IPv4Gateway", "IPv6Address", "IPv6Gateway",
            "IPv6MaskLen", "DHCP", "CommandPort", "HttpPort", "DSPVersion", "BootTime",
            "SoftwareVersion", "Activated", "PasswordResetModeSecond", "PasswordResetAbility",
            "SupportSecurityQuestion", "SupportHCPlatform", "HCPlatformEnable", "Encoder",
            "OEMInfo", "AnalogChannelNum", "DigitalChannelNum", "SDKOverTLSPort",
            "SDKServerStatus"])
        fields = sadp_probe.children_text(root)
        self.assertEqual(fields["Uuid"], "ABC-123")
        self.assertEqual(fields["Types"], "inquiry")
        self.assertEqual(fields["DeviceDescription"], "DS-TCG406-E")
        self.assertEqual(fields["MAC"], "bc-ad-28-77-00-21")
        self.assertEqual(fields["IPv4Address"], "192.168.77.21")
        self.assertEqual(fields["IPv4Gateway"], "192.168.77.1")
        self.assertEqual(fields["DHCP"], "false")
        self.assertEqual(fields["Activated"], "true")
        self.assertEqual((fields["HttpPort"], fields["CommandPort"]), ("80", "8000"))
        self.assertEqual(fields["SoftwareVersion"], "V5.7.10build 231010")
        inactive = make_config(activated=False)
        reply = fh.sadp_probe_match(inactive, "X", "t", 80, 8000)
        self.assertIn("<Activated>false</Activated>", reply)

    def test_sadp_datagram_handling(self):
        sender = ("192.168.77.5", 37020)

        def answer(payload):
            return fh.handle_sadp_datagram(self.config, payload, sender, "t", 80, 8000)

        for types in ("inquiry", "inquiry_v32"):
            reply = answer(sadp_probe.build_sadp_probe("UUID-1", types))
            self.assertEqual(sadp_probe.parse_sadp_reply(reply)["Uuid"], "UUID-1")
        prefixed = b"\x00\x01binary-header" + sadp_probe.build_sadp_probe("UUID-2")
        self.assertIsNotNone(answer(prefixed))
        self.assertIsNone(answer(b"<?xml version='1.0'?><Probe><Uuid>1</Uuid><Types>update"
                                 b"</Types></Probe>"))
        self.assertIsNone(answer(fh.sadp_probe_match(self.config, "x", "t", 80, 8000)
                                 .encode()))
        self.assertIsNone(answer(b"\xff\xfe garbage"))
        self.assertIsNone(answer(b"<Probe><Uuid>unterminated"))

    def test_ws_discovery_datagram_handling(self):
        sender = ("192.168.77.5", 50000)
        reply = fh.handle_ws_discovery_datagram(self.config, sadp_probe.build_ws_probe("m-1"),
                                                sender, 80)
        matches = sadp_probe.parse_ws_reply(reply)
        self.assertEqual(len(matches), 1)
        match = matches[0]
        self.assertEqual(match["relates_to"], "urn:uuid:m-1")
        self.assertEqual(match["endpoint"], "urn:uuid:" + self.config.onvif_endpoint_uuid)
        self.assertIn("dn:NetworkVideoTransmitter", match["types"])
        self.assertIn("onvif://www.onvif.org/hardware/DS-TCG406-E", match["scopes"])
        self.assertIn("onvif://www.onvif.org/name/HIKVISION%20DS-TCG406-E", match["scopes"])
        self.assertIn("onvif://www.onvif.org/type/video_encoder", match["scopes"])
        self.assertEqual(match["xaddrs"], ["http://192.168.77.21/onvif/device_service"])
        self.assertEqual(sadp_probe.scope_value(match["scopes"], "name"), "HIKVISION DS-TCG406-E")
        on_port = fh.handle_ws_discovery_datagram(self.config, sadp_probe.build_ws_probe("m"),
                                                  sender, 8080)
        self.assertIn(b"http://192.168.77.21:8080/onvif/device_service", on_port)
        printer = sadp_probe.build_ws_probe("m-2").replace(b"dn:NetworkVideoTransmitter",
                                                           b"wsdp:Printer")
        self.assertIsNone(fh.handle_ws_discovery_datagram(self.config, printer, sender, 80))
        scoped = sadp_probe.build_ws_probe("m-3").replace(
            b"</d:Types>", b"</d:Types><d:Scopes>onvif://www.onvif.org/hardware</d:Scopes>")
        self.assertIsNotNone(fh.handle_ws_discovery_datagram(self.config, scoped, sender, 80))
        unmatched = sadp_probe.build_ws_probe("m-4").replace(
            b"</d:Types>", b"</d:Types><d:Scopes>onvif://www.onvif.org/hardware/DS-2CD</d:Scopes>")
        self.assertIsNone(fh.handle_ws_discovery_datagram(self.config, unmatched, sender, 80))
        self.assertIsNone(fh.handle_ws_discovery_datagram(self.config, b"<Probe/>", sender, 80))

    def test_isapi_documents(self):
        info = ET.fromstring(fh.device_info_xml(self.config).encode())
        self.assertEqual(info.tag, HIK + "DeviceInfo")
        for name, value in (("model", "DS-TCG406-E"), ("macAddress", "bc:ad:28:77:00:21"),
                            ("serialNumber", self.config.serial), ("deviceType", "IPCamera"),
                            ("deviceID", self.config.device_id), ("firmwareVersion", "V5.7.10"),
                            ("deviceName", "SIM unit")):
            self.assertEqual(info.findtext(HIK + name), value)
        channel = ET.fromstring(fh.streaming_channel_xml(self.config,
                                                         self.config.channel(102)).encode())
        self.assertEqual(channel.tag, HIK + "StreamingChannel")
        self.assertEqual(channel.findtext(HIK + "id"), "102")
        video = channel.find(HIK + "Video")
        self.assertEqual(video.findtext(HIK + "videoCodecType"), "H.265")
        self.assertEqual(video.findtext(HIK + "videoResolutionWidth"), "304")
        self.assertEqual(video.findtext(HIK + "videoResolutionHeight"), "540")
        self.assertEqual(video.findtext(HIK + "maxFrameRate"), "3000")
        self.assertEqual(video.findtext(HIK + "H265Profile"), "Main")
        listing = ET.fromstring(fh.streaming_channel_list_xml(self.config).encode())
        self.assertEqual(len(listing.findall(HIK + "StreamingChannel")), 2)
        traffic = ET.fromstring(fh.traffic_capabilities_xml(self.config).encode())
        self.assertEqual(traffic.findtext(HIK + "isSupportANPR"), "true")
        self.assertEqual(traffic.findtext(HIK + "isSupportVehicleDetection"), "true")
        status = ET.fromstring(fh.response_status_xml("/x", 4, "Invalid Operation",
                                                      "notSupport").encode())
        self.assertEqual(status.findtext(HIK + "subStatusCode"), "notSupport")
        for document in (fh.network_interfaces_xml(self.config), fh.integrate_xml(self.config),
                         fh.time_xml(self.config), fh.user_check_xml(401, 10, 2)):
            ET.fromstring(document.encode())

    def test_alert_documents(self):
        moment = datetime.datetime(2026, 10, 8, 20, 1, 2, 345000, tzinfo=datetime.timezone.utc)
        event = fh.AnprEvent("152JTA02", 3, moment, "1b4e28ba-2fa1-11d2-883f-0016d3cca427")
        alert = ET.fromstring(fh.anpr_alert_xml(
            self.config, 80, event, ["licensePlatePicture.jpg", "detectionPicture.jpg"]).encode())
        self.assertEqual(alert.tag, HIK + "EventNotificationAlert")
        self.assertEqual(alert.get("version"), "2.0")
        self.assertEqual(alert.findtext(HIK + "eventType"), "ANPR")
        self.assertEqual(alert.findtext(HIK + "eventState"), "active")
        self.assertEqual(alert.findtext(HIK + "dateTime"), "2026-10-09T01:01:02+05:00")
        self.assertEqual(alert.findtext(HIK + "UUID"), event.uuid)
        self.assertEqual(alert.findtext(HIK + "ipAddress"), "192.168.77.21")
        anpr = alert.find(HIK + "ANPR")
        self.assertEqual(anpr.findtext(HIK + "licensePlate"), "152JTA02")
        self.assertEqual(anpr.findtext(HIK + "originalLicensePlate"), "152JTA02")
        self.assertEqual(anpr.findtext(HIK + "country"), "30")
        self.assertEqual(anpr.findtext(HIK + "direction"), "forward")
        self.assertEqual(len(anpr.findtext(HIK + "plateCharBelieve").split(",")), 8)
        pictures = anpr.find(HIK + "pictureInfoList").findall(HIK + "pictureInfo")
        self.assertEqual([p.findtext(HIK + "type") for p in pictures],
                         ["licensePlatePicture", "detectionPicture"])
        self.assertEqual(pictures[0].findtext(HIK + "absTime"), "20261009010102345")
        rect = pictures[1].find(HIK + "plateRect")
        self.assertEqual([rect.findtext(HIK + k) for k in ("X", "Y", "width", "height")],
                         ["338", "338", "117", "44"])
        heartbeat = ET.fromstring(fh.heartbeat_alert_xml(self.config, 80, moment).encode())
        self.assertEqual(heartbeat.findtext(HIK + "eventType"), "videoloss")
        self.assertEqual(heartbeat.findtext(HIK + "eventState"), "inactive")
        self.assertEqual(heartbeat.findtext(HIK + "activePostCount"), "0")

    def test_multipart_part_layout(self):
        part = fh.multipart_part([("Content-Type", "image/jpeg")], b"\xff\xd8\xff")
        self.assertEqual(part, b"--boundary\r\nContent-Type: image/jpeg\r\n"
                               b"Content-Length: 3\r\n\r\n\xff\xd8\xff\r\n")

    def test_onvif_documents(self):
        moment = datetime.datetime(2026, 10, 8, 20, 1, 2, tzinfo=datetime.timezone.utc)
        answer = fh.onvif_date_time_xml(self.config, moment).encode()
        self.assertEqual(sadp_probe.device_utc(answer), datetime.datetime(2026, 10, 8, 20, 1, 2))
        self.assertIn(b"<tt:LocalDateTime><tt:Time><tt:Hour>1</tt:Hour>", answer)
        info = ET.fromstring(fh.onvif_device_information_xml(self.config).encode())
        self.assertEqual(fh.text_of(info, "Model"), "DS-TCG406-E")
        self.assertEqual(fh.text_of(info, "SerialNumber"), self.config.serial)
        fault = ET.fromstring(fh.soap_fault("Sender", "ter:NotAuthorized", "no").encode())
        self.assertEqual(fh.text_of(fh.find_local(fault, "Subcode"), "Value"),
                         "ter:NotAuthorized")


class RtspLogWatcherTests(unittest.TestCase):
    def test_counts_mediamtx_auth_failures(self):
        guard = fh.LoginGuard(5, 1800, 1800)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "mediamtx.log")
            with open(path, "w") as handle:
                handle.write("2026/10/08 20:51:53 WAR [RTSP] [conn 10.0.0.9:1] failed to "
                             "authenticate: authentication failed\n")
            watcher = fh.RtspLogWatcher(path, guard)  # starts at the end: old lines ignored
            watcher.poll_once()
            self.assertEqual(guard.failures("10.0.0.9"), 0)
            with open(path, "a") as handle:
                handle.write("2026/10/08 20:51:56 INF [RTSP] [conn 10.0.0.5:2] opened\n"
                             "2026/10/08 20:51:57 WAR [RTSP] [conn 10.0.0.5:2] failed to "
                             "authenticate: authentication failed\n"
                             "2026/10/08 20:51:58 WAR [RTSP] [conn 10.0.0.5:3] failed to au")
            watcher.poll_once()
            self.assertEqual(guard.failures("10.0.0.5"), 1)
            with open(path, "a") as handle:
                handle.write("thenticate: authentication failed\n")
            watcher.poll_once()
            self.assertEqual(guard.failures("10.0.0.5"), 2)
            with open(path, "w") as handle:  # truncated by a new `sim.sh up`
                handle.write("WAR [RTSP] [conn 10.0.0.6:4] failed to authenticate: x\n")
            watcher.poll_once()
            self.assertEqual(guard.failures("10.0.0.6"), 1)


# ---------------------------------------------------------------------------------------------
# Running camera on 127.0.0.1


class RunningCamera(object):
    def __init__(self, **overrides):
        self.collector = LogCollector()
        fh.LOG.addHandler(self.collector)
        fh.LOG.setLevel(logging.INFO)
        self.camera = fh.FakeCamera(make_config(**overrides))
        self.camera.start()

    def close(self):
        self.camera.stop()
        fh.LOG.removeHandler(self.collector)

    def request(self, method, path, headers=None, body=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.camera.http_port, timeout=5)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def authed(self, method, path, password=PASSWORD, username="admin"):
        status, headers, _ = self.request(method, path)
        assert status == 401, status
        auth = digest_header(method, path, headers["WWW-Authenticate"], username, password)
        return self.request(method, path, {"Authorization": auth})


class HttpServiceTests(unittest.TestCase):
    def setUp(self):
        self.cam = RunningCamera()

    def tearDown(self):
        self.cam.close()
        self.assertNotIn(PASSWORD, self.cam.collector.text())

    def test_index_page_and_server_header(self):
        status, headers, body = self.cam.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Server"], "webserver")
        self.assertIn(b"SIMULATED", body)
        self.assertEqual(self.cam.request("GET", "/nothing")[0], 404)

    def test_isapi_requires_digest(self):
        status, headers, body = self.cam.request("GET", "/ISAPI/System/deviceInfo")
        self.assertEqual(status, 401)
        self.assertEqual(headers["Server"], "webserver")
        self.assertRegex(headers["WWW-Authenticate"],
                         r'^Digest qop="auth", realm="IP Camera\(SIM21\)", nonce="[0-9a-f]+", '
                         r'stale="FALSE"$')
        self.assertIn(b"<lockStatus>unlock</lockStatus>", body)
        self.assertEqual(self.cam.camera.guard.failures("127.0.0.1"), 0)

    def test_device_info_with_digest(self):
        status, headers, body = self.cam.authed("GET", "/ISAPI/System/deviceInfo")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Content-Type"], 'application/xml; charset="UTF-8"')
        info = ET.fromstring(body)
        self.assertEqual(info.findtext(HIK + "model"), "DS-TCG406-E")
        self.assertIn("auth=ok user='admin'", self.cam.collector.text())

    def test_stdlib_digest_client_interoperates(self):
        url = "http://127.0.0.1:%d/ISAPI/Streaming/channels/101" % self.cam.camera.http_port
        manager = urllib.request.HTTPPasswordMgrWithDefaultRealm()
        manager.add_password(None, url, "admin", PASSWORD)
        opener = urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(manager))
        with opener.open(url, timeout=5) as response:
            channel = ET.fromstring(response.read())
        video = channel.find(HIK + "Video")
        self.assertEqual(video.findtext(HIK + "videoCodecType"), "H.264")
        self.assertEqual(video.findtext(HIK + "videoResolutionWidth"), "608")
        self.assertEqual(video.findtext(HIK + "maxFrameRate"), "3000")

    def test_channels_capabilities_and_unknown_resources(self):
        self.assertEqual(self.cam.authed("GET", "/ISAPI/Streaming/channels/102")[0], 200)
        self.assertEqual(self.cam.authed("GET", "/ISAPI/Streaming/channels")[0], 200)
        status, _, body = self.cam.authed("GET", "/ISAPI/Streaming/channels/103")
        self.assertEqual(status, 404)
        self.assertIn(b"<subStatusCode>notSupport</subStatusCode>", body)
        status, _, body = self.cam.authed("GET", "/ISAPI/Traffic/capabilities")
        self.assertEqual(status, 200)
        self.assertIn(b"<isSupportANPR>true</isSupportANPR>", body)
        for path in ("/ISAPI/System/Network/interfaces", "/ISAPI/System/Network/Integrate",
                     "/ISAPI/System/time", "/ISAPI/Security/userCheck"):
            self.assertEqual(self.cam.authed("GET", path)[0], 200, path)
        self.assertEqual(self.cam.authed("GET", "/ISAPI/System/reboot")[0], 404)

    def test_writes_are_rejected_and_logged(self):
        status, _, body = self.cam.authed("PUT", "/ISAPI/System/Network/Integrate")
        self.assertEqual(status, 403)
        self.assertIn(b"Invalid Operation", body)
        self.assertIn("WRITE_REJECTED", self.cam.collector.text())

    def test_wrong_password_is_counted_and_locks(self):
        for attempt in range(1, 5):
            status, _, body = self.cam.authed("GET", "/ISAPI/System/deviceInfo", "wrong")
            self.assertEqual(status, 401)
            self.assertIn(("<retryLoginTime>%d</retryLoginTime>" % (5 - attempt)).encode(), body)
        self.assertNotIn("LOCKED", self.cam.collector.text())
        status, _, body = self.cam.authed("GET", "/ISAPI/System/deviceInfo", "wrong")
        self.assertEqual(status, 401)
        self.assertIn(b"<lockStatus>lock</lockStatus>", body)
        self.assertIn("LOCKED ip=127.0.0.1 after 5 failed logins", self.cam.collector.text())
        # The right password no longer helps from this address.
        status, _, body = self.cam.request("GET", "/ISAPI/System/deviceInfo")
        self.assertEqual(status, 401)
        self.assertIn(b"<lockStatus>lock</lockStatus>", body)
        self.assertIn("LOCKED reject ip=127.0.0.1", self.cam.collector.text())
        self.assertIn("AUTH_FAIL ip=127.0.0.1 source=isapi user='admin' reason=wrong-response "
                      "failures=1/5", self.cam.collector.text())

    def test_stale_nonce_is_not_a_failed_login(self):
        auth = digest_header("GET", "/ISAPI/System/deviceInfo",
                             'Digest realm="IP Camera(SIM21)", nonce="feedface"', "admin",
                             PASSWORD)
        status, headers, _ = self.cam.request("GET", "/ISAPI/System/deviceInfo",
                                              {"Authorization": auth})
        self.assertEqual(status, 401)
        self.assertIn('stale="TRUE"', headers["WWW-Authenticate"])
        self.assertEqual(self.cam.camera.guard.failures("127.0.0.1"), 0)

    def test_onvif_disabled_returns_404(self):
        body = soap("<tds:GetSystemDateAndTime/>").encode()
        self.assertEqual(self.cam.request("POST", "/onvif/device_service", body=body)[0], 404)


class ActivationTests(unittest.TestCase):
    def test_inactive_camera_rejects_everything(self):
        cam = RunningCamera(activated=False)
        try:
            self.assertEqual(cam.request("GET", "/")[0], 401)
            status, _, body = cam.authed("GET", "/ISAPI/System/deviceInfo")
            self.assertEqual(status, 401)
            self.assertIn(b"<isActivated>false</isActivated>", body)
            self.assertEqual(cam.camera.guard.failures("127.0.0.1"), 0)
        finally:
            cam.close()


class OnvifServiceTests(unittest.TestCase):
    def setUp(self):
        self.cam = RunningCamera(onvif=True, onvif_username="onvifuser",
                                 onvif_password=ONVIF_PASSWORD)

    def tearDown(self):
        self.cam.close()
        self.assertNotIn(ONVIF_PASSWORD, self.cam.collector.text())

    def post(self, inner, header=""):
        return self.cam.request("POST", "/onvif/device_service",
                                {"Content-Type": "application/soap+xml"},
                                soap(inner, header).encode())

    def test_date_time_needs_no_credentials(self):
        status, headers, body = self.post("<tds:GetSystemDateAndTime/>")
        self.assertEqual(status, 200)
        self.assertTrue(headers["Content-Type"].startswith("application/soap+xml"))
        self.assertIsNotNone(sadp_probe.device_utc(body))

    def test_device_information_needs_a_valid_token(self):
        status, _, body = self.post("<tds:GetDeviceInformation/>")
        self.assertEqual(status, 401)
        self.assertIn(b"ter:NotAuthorized", body)
        self.assertEqual(self.cam.camera.guard.failures("127.0.0.1"), 0)
        created = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
        good = sadp_probe.username_token("onvifuser", ONVIF_PASSWORD, created)
        status, _, body = self.post("<tds:GetDeviceInformation/>", good)
        self.assertEqual(status, 200)
        self.assertEqual(fh.text_of(ET.fromstring(body), "Manufacturer"), "HIKVISION")
        # The ISAPI password is not the ONVIF account's.
        wrong = sadp_probe.username_token("onvifuser", PASSWORD, created)
        self.assertEqual(self.post("<tds:GetDeviceInformation/>", wrong)[0], 401)
        self.assertEqual(self.cam.camera.guard.failures("127.0.0.1"), 1)
        self.assertIn("AUTH_FAIL ip=127.0.0.1 source=onvif", self.cam.collector.text())

    def test_unsupported_and_malformed_requests(self):
        self.assertEqual(self.post("<tds:GetCapabilities/>")[0], 500)
        self.assertEqual(self.cam.request("POST", "/onvif/device_service", body=b"<x")[0], 400)
        self.assertEqual(self.cam.request("GET", "/onvif/device_service")[0], 405)
        self.assertEqual(self.cam.request("POST", "/onvif/Media", body=b"<x/>")[0], 400)

    def test_onvif_info_client(self):
        os.environ["SIM_TEST_ONVIF_USER"] = "onvifuser"
        os.environ["SIM_TEST_ONVIF_PASS"] = ONVIF_PASSWORD
        try:
            output = io.StringIO()
            with redirect_stdout(output):
                code = sadp_probe.main(["onvif-info", "127.0.0.1:%d" % self.cam.camera.http_port,
                                        "--username-env", "SIM_TEST_ONVIF_USER",
                                        "--password-env", "SIM_TEST_ONVIF_PASS"])
            self.assertEqual(code, 0, output.getvalue())
            self.assertIn("model=DS-TCG406-E", output.getvalue())
            self.assertNotIn(ONVIF_PASSWORD, output.getvalue())
            os.environ["SIM_TEST_ONVIF_PASS"] = "wrong"
            with redirect_stdout(io.StringIO()):
                code = sadp_probe.main(["onvif-info", "127.0.0.1:%d" % self.cam.camera.http_port,
                                        "--username-env", "SIM_TEST_ONVIF_USER",
                                        "--password-env", "SIM_TEST_ONVIF_PASS"])
            self.assertEqual(code, 2)
        finally:
            del os.environ["SIM_TEST_ONVIF_USER"]
            del os.environ["SIM_TEST_ONVIF_PASS"]


class ChunkedMultipartReader(object):
    """Reads the alertStream body: de-chunks and splits on the boundary."""

    def __init__(self, sock_file):
        self._file = sock_file
        self._buffer = b""

    def _read_chunk(self):
        size_line = self._file.readline()
        size = int(size_line.strip(), 16)
        data = self._file.read(size)
        self._file.readline()
        return data

    def next_part(self):
        delimiter = b"--boundary\r\n"
        while True:
            start = self._buffer.find(delimiter)
            if start >= 0:
                header_end = self._buffer.find(b"\r\n\r\n", start)
                if header_end >= 0:
                    headers = {}
                    for line in self._buffer[start + len(delimiter):header_end].split(b"\r\n"):
                        name, _, value = line.decode().partition(":")
                        headers[name.strip().lower()] = value.strip()
                    length = int(headers["content-length"])
                    body_start = header_end + 4
                    if len(self._buffer) >= body_start + length + 2:
                        body = self._buffer[body_start:body_start + length]
                        assert self._buffer[body_start + length:body_start + length + 2] == b"\r\n"
                        self._buffer = self._buffer[body_start + length + 2:]
                        return headers, body
            self._buffer += self._read_chunk()


class StreamConnection(object):
    """The socket and its buffered reader: the descriptor closes only when both are closed."""

    def __init__(self, sock, stream):
        self._sock = sock
        self._stream = stream

    def close(self):
        self._stream.close()
        self._sock.close()


class AlertStreamTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.picture = os.path.join(self.directory.name, "detectionPicture.jpg")
        with open(self.picture, "wb") as handle:
            handle.write(b"\xff\xd8\xff\xe0fake-jpeg\xff\xd9")
        self.cam = RunningCamera(heartbeat_interval=0.3, anpr_pictures=[self.picture])

    def tearDown(self):
        self.cam.close()
        self.directory.cleanup()

    def open_stream(self):
        path = "/ISAPI/Event/notification/alertStream"
        status, headers, _ = self.cam.request("GET", path)
        self.assertEqual(status, 401)
        sock = socket.create_connection(("127.0.0.1", self.cam.camera.http_port), timeout=5)
        auth = digest_header("GET", path, headers["WWW-Authenticate"], "admin", PASSWORD)
        sock.sendall(("GET %s HTTP/1.1\r\nHost: cam\r\nAuthorization: %s\r\n\r\n"
                      % (path, auth)).encode())
        stream = sock.makefile("rb")
        status_line = stream.readline()
        self.assertIn(b" 200 ", status_line)
        response_headers = {}
        while True:
            line = stream.readline().strip()
            if not line:
                break
            name, _, value = line.decode().partition(":")
            response_headers[name.lower()] = value.strip()
        self.assertEqual(response_headers["content-type"], "multipart/mixed; boundary=boundary")
        self.assertEqual(response_headers["transfer-encoding"], "chunked")
        return StreamConnection(sock, stream), ChunkedMultipartReader(stream)

    def test_heartbeats_then_anpr_with_picture(self):
        sock, reader = self.open_stream()
        try:
            for _ in range(2):
                headers, body = reader.next_part()
                self.assertEqual(headers["content-type"], 'application/xml; charset="UTF-8"')
                alert = ET.fromstring(body)
                self.assertEqual(alert.findtext(HIK + "eventType"), "videoloss")
                self.assertEqual(alert.findtext(HIK + "eventState"), "inactive")
            self.cam.camera.anpr.trigger()
            while True:
                headers, body = reader.next_part()
                alert = ET.fromstring(body)
                if alert.findtext(HIK + "eventType") == "ANPR":
                    break
            self.assertIn('name="anpr.xml"', headers["content-disposition"])
            anpr = alert.find(HIK + "ANPR")
            self.assertEqual(anpr.findtext(HIK + "licensePlate"), "152JTA02")
            self.assertEqual(anpr.findtext(HIK + "country"), "30")
            self.assertEqual(alert.findtext(HIK + "picNum"), "1")
            headers, body = reader.next_part()
            self.assertEqual(headers["content-type"], "image/jpeg")
            self.assertIn('filename="detectionPicture.jpg"', headers["content-disposition"])
            self.assertEqual(body, b"\xff\xd8\xff\xe0fake-jpeg\xff\xd9")
        finally:
            sock.close()
        deadline = time.monotonic() + 3
        while "alertStream closed after 1 ANPR event(s)" not in self.cam.collector.text():
            self.assertLess(time.monotonic(), deadline, self.cam.collector.text())
            time.sleep(0.05)

    def test_periodic_anpr_events_and_heartbeats(self):
        # Heartbeats keep their period even while ANPR events arrive more often.
        self.cam.close()
        self.cam = RunningCamera(heartbeat_interval=0.5, anpr_interval=0.15)
        sock, reader = self.open_stream()
        try:
            self.assertEqual(ET.fromstring(reader.next_part()[1]).findtext(HIK + "eventType"),
                             "videoloss")
            started = time.monotonic()
            uuids = set()
            heartbeats = 0
            while heartbeats < 2:
                alert = ET.fromstring(reader.next_part()[1])
                if alert.findtext(HIK + "eventType") == "videoloss":
                    heartbeats += 1
                    continue
                self.assertEqual(alert.findtext(HIK + "eventType"), "ANPR")
                self.assertEqual(alert.findtext(HIK + "picNum"), "0")
                uuids.add(alert.findtext(HIK + "UUID"))
            self.assertLess(time.monotonic() - started, 2.0)
            self.assertGreaterEqual(len(uuids), 4)
        finally:
            sock.close()

    def test_stream_ends_cleanly_when_camera_stops(self):
        sock, reader = self.open_stream()
        try:
            reader.next_part()
            self.cam.camera.bus.close()
            tail = b""
            while True:
                chunk = reader._read_chunk()
                if not chunk:
                    break
                tail += chunk
            self.assertTrue(tail.endswith(b"--boundary--\r\n"))
        finally:
            sock.close()


class DiscoveryOverUdpTests(unittest.TestCase):
    def setUp(self):
        self.cam = RunningCamera(onvif=True)

    def tearDown(self):
        self.cam.close()

    def test_sadp_and_ws_discovery_answer_by_unicast(self):
        output = io.StringIO()
        with redirect_stdout(output):
            code = sadp_probe.main(["sadp", "--target", "127.0.0.1:%d" % self.cam.camera.sadp_port,
                                    "--bind-port", "0", "--timeout", "0.6",
                                    "--expect", "192.168.77.21"])
        self.assertEqual(code, 0, output.getvalue())
        self.assertIn("mac=bc-ad-28-77-00-21 model=DS-TCG406-E", output.getvalue())
        self.assertIn("activated=true", output.getvalue())
        output = io.StringIO()
        with redirect_stdout(output):
            code = sadp_probe.main(["wsd", "--target", "127.0.0.1:%d" % self.cam.camera.wsd_port,
                                    "--timeout", "0.6", "--expect", "127.0.0.1"])
        self.assertEqual(code, 0, output.getvalue())
        self.assertIn("hardware=DS-TCG406-E", output.getvalue())
        with redirect_stdout(io.StringIO()):
            code = sadp_probe.main(["sadp", "--target", "127.0.0.1:%d" % self.cam.camera.sadp_port,
                                    "--bind-port", "0", "--timeout", "0.4",
                                    "--expect", "192.168.77.99"])
        self.assertEqual(code, 1)

    def test_sdk_port_accepts_connections(self):
        sock = socket.create_connection(("127.0.0.1", self.cam.camera.sdk_port), timeout=2)
        sock.sendall(b"\x00\x00\x00\x20")
        sock.close()
        deadline = time.monotonic() + 3
        while "SDK port connection from 127.0.0.1 closed (4 bytes" not in \
                self.cam.collector.text():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.05)


class CommandLineTests(unittest.TestCase):
    def test_config_from_args(self):
        args = fh.build_parser().parse_args([
            "--name", "cam9", "--ip", "192.168.77.29", "--mac", "bc-ad-28-77-00-29",
            "--netmask", "255.255.255.0", "--gateway", "192.168.77.1", "--password", "x",
            "--onvif", "on", "--activated", "false", "--channel", "101,H.265,608x1080,3000,60",
            "--anpr-plate", "001AAA01", "--anpr-plate-rect", "1,2,3,4", "--utc-offset", "+06:00"])
        config = fh.config_from_args(args)
        self.assertEqual((config.ip, config.mac, config.onvif, config.activated),
                         ("192.168.77.29", "bc:ad:28:77:00:29", True, False))
        self.assertEqual(config.channel(101).codec, "H.265")
        self.assertIsNone(config.channel(102))
        self.assertEqual(config.anpr_plate_rect, (1, 2, 3, 4))
        self.assertEqual(config.realm, "IP Camera(SIM29)")

    def test_password_is_required_and_env_is_read(self):
        args = fh.build_parser().parse_args(["--ip", "10.0.0.1", "--mac", "02:00:00:00:00:01"])
        args.password = ""
        with self.assertRaises(SystemExit):
            fh.config_from_args(args)
        os.environ["FAKE_HIK_PASSWORD"] = "from-env"
        os.environ["FAKE_HIK_CHANNELS"] = "101,H.264,608x1080,3000;102,H.264,304x540,3000"
        try:
            args = fh.build_parser().parse_args(["--ip", "10.0.0.1", "--mac",
                                                 "02:00:00:00:00:01"])
            config = fh.config_from_args(args)
            self.assertEqual(config.password, "from-env")
            self.assertEqual([c.channel_id for c in config.channels], [101, 102])
        finally:
            del os.environ["FAKE_HIK_PASSWORD"]
            del os.environ["FAKE_HIK_CHANNELS"]

    def test_sadp_probe_defaults_to_sadp(self):
        args = sadp_probe.build_parser().parse_args(["sadp"])
        self.assertEqual((args.bind_port, args.timeout), (37020, 3.0))


if __name__ == "__main__":
    unittest.main()
