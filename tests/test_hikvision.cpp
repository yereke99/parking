#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "anpr/common/json.hpp"
#include "anpr/hikvision/isapi.hpp"
#include "anpr/hikvision/native_anpr.hpp"
#include "anpr/hikvision/onvif.hpp"
#include "anpr/hikvision/sadp.hpp"
#include "anpr/hikvision/xml_lite.hpp"
#include "anpr/net/crypto.hpp"
#include "test_framework.hpp"

using anpr::hikvision::MultipartParser;
using anpr::hikvision::NativeAnprSupport;
using anpr::net::Ipv4;

namespace {

Ipv4 ip(const char* text) {
    return anpr::net::parseIpv4(text).value_or(Ipv4{});
}

/// A SADP answer as a DS-TCG406-E in factory state sends it (field names from the open-source
/// SADP implementations in the research notes).
const std::string kProbeMatch = R"(<?xml version="1.0" encoding="UTF-8"?>
<ProbeMatch>
<Uuid>4C2D1F3A-7B8E-4D2C-9A61-3E5F7B9C1D2E</Uuid>
<Types>inquiry</Types>
<DeviceType>139267</DeviceType>
<DeviceDescription>DS-TCG406-E</DeviceDescription>
<DeviceSN>DS-TCG406-E20240512AAWRFA1234567</DeviceSN>
<CommandPort>8000</CommandPort>
<HttpPort>80</HttpPort>
<MAC>c4-2f-90-a7-b5-d1</MAC>
<IPv4Address>192.168.1.64</IPv4Address>
<IPv4SubnetMask>255.255.255.0</IPv4SubnetMask>
<IPv4Gateway>192.168.1.1</IPv4Gateway>
<IPv6Address>::</IPv6Address>
<IPv6Gateway>::</IPv6Gateway>
<IPv6MaskLen>64</IPv6MaskLen>
<DHCP>true</DHCP>
<AnalogChannelNum>0</AnalogChannelNum>
<DigitalChannelNum>1</DigitalChannelNum>
<SoftwareVersion>V5.6.10build 230721</SoftwareVersion>
<DSPVersion>V7.3 build 230721</DSPVersion>
<BootTime>2026-10-08 07:12:03</BootTime>
<Activated>false</Activated>
<PasswordResetAbility>true</PasswordResetAbility>
<PasswordResetModeSecond>true</PasswordResetModeSecond>
<SupportSecurityQuestion>true</SupportSecurityQuestion>
<SupportHCPlatform>true</SupportHCPlatform>
<HCPlatformEnable>false</HCPlatformEnable>
<OEMInfo>N/A</OEMInfo>
<SDKOverTLSPort>8443</SDKOverTLSPort>
<SDKServerStatus>true</SDKServerStatus>
</ProbeMatch>
)";

const std::string kWsProbeMatches = R"(<?xml version="1.0" encoding="UTF-8"?>
<env:Envelope xmlns:env="http://www.w3.org/2003/05/soap-envelope"
 xmlns:wsadis="http://schemas.xmlsoap.org/ws/2005/04/discovery"
 xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing"
 xmlns:dn="http://www.onvif.org/ver10/network/wsdl"
 xmlns:tds="http://www.onvif.org/ver10/device/wsdl">
<env:Header>
<wsa:MessageID>urn:uuid:1d3c5e7a-0000-4000-8000-c42f90a7b5d1</wsa:MessageID>
<wsa:RelatesTo>uuid:0b6f5a1e-2c3d-4e5f-8a9b-0c1d2e3f4a5b</wsa:RelatesTo>
<wsa:To env:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
<wsa:Action env:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>
<wsadis:AppSequence InstanceId="1696270812" MessageNumber="7"/>
</env:Header>
<env:Body>
<wsadis:ProbeMatches>
<wsadis:ProbeMatch>
<wsa:EndpointReference>
<wsa:Address>urn:uuid:3fa1fe68-b915-4053-a3e1-c42f90a7b5d1</wsa:Address>
</wsa:EndpointReference>
<wsadis:Types>dn:NetworkVideoTransmitter tds:Device</wsadis:Types>
<wsadis:Scopes>onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/Profile/G onvif://www.onvif.org/Profile/T onvif://www.onvif.org/hardware/DS-TCG406-E onvif://www.onvif.org/name/HIKVISION%20DS-TCG406-E onvif://www.onvif.org/location/city/hangzhou</wsadis:Scopes>
<wsadis:XAddrs>http://[fe80::c62f:90ff:fea7:b5d1]/onvif/device_service http://192.168.1.64/onvif/device_service</wsadis:XAddrs>
<wsadis:MetadataVersion>10</wsadis:MetadataVersion>
</wsadis:ProbeMatch>
</wsadis:ProbeMatches>
</env:Body>
</env:Envelope>
)";

const std::string kDeviceInfo = R"(<?xml version="1.0" encoding="UTF-8"?>
<DeviceInfo version="2.0" xmlns="http://www.hikvision.com/ver20/XMLSchema">
<deviceName>Entrance</deviceName>
<deviceID>6aff4000-1000-11b2-8000-c42f90a7b5d1</deviceID>
<deviceDescription>IPCamera</deviceDescription>
<deviceLocation>hangzhou</deviceLocation>
<systemContact>Hikvision.China</systemContact>
<model>DS-TCG406-E</model>
<serialNumber>DS-TCG406-E20240512AAWRFA1234567</serialNumber>
<macAddress>C4:2F:90:A7:B5:D1</macAddress>
<firmwareVersion>V5.7.18</firmwareVersion>
<firmwareReleasedDate>build 240826</firmwareReleasedDate>
<encoderVersion>V7.3</encoderVersion>
<encoderReleasedDate>build 240826</encoderReleasedDate>
<deviceType>IPCamera</deviceType>
<telecontrolID>88</telecontrolID>
<hardwareVersion>0x0</hardwareVersion>
<manufacturer>hikvision</manufacturer>
</DeviceInfo>
)";

const std::string kStreamingChannel = R"(<?xml version="1.0" encoding="UTF-8"?>
<StreamingChannel version="2.0" xmlns="http://www.hikvision.com/ver20/XMLSchema">
<id>101</id>
<channelName>Entrance</channelName>
<enabled>true</enabled>
<Transport>
<maxPacketSize>1000</maxPacketSize>
<ControlProtocolList>
<ControlProtocol><streamingTransport>RTSP</streamingTransport></ControlProtocol>
<ControlProtocol><streamingTransport>HTTP</streamingTransport></ControlProtocol>
</ControlProtocolList>
<Security><enabled>true</enabled><certificateType>digest</certificateType></Security>
</Transport>
<Video>
<enabled>true</enabled>
<videoInputChannelID>1</videoInputChannelID>
<videoCodecType>H.265</videoCodecType>
<videoScanType>progressive</videoScanType>
<videoResolutionWidth>2688</videoResolutionWidth>
<videoResolutionHeight>1520</videoResolutionHeight>
<videoQualityControlType>VBR</videoQualityControlType>
<constantBitRate>4096</constantBitRate>
<fixedQuality>60</fixedQuality>
<vbrUpperCap>4096</vbrUpperCap>
<maxFrameRate>2500</maxFrameRate>
<keyFrameInterval>2000</keyFrameInterval>
<H265Profile>Main</H265Profile>
<GovLength>50</GovLength>
</Video>
<Audio><enabled>false</enabled><audioCompressionType>G.711ulaw</audioCompressionType></Audio>
</StreamingChannel>
)";

/// The captured ANPR alert from the research notes, wrapped in its EventNotificationAlert.
const std::string kAnprAlert = R"(<?xml version="1.0" encoding="UTF-8"?>
<EventNotificationAlert version="2.0" xmlns="http://www.hikvision.com/ver20/XMLSchema">
<ipAddress>192.168.1.64</ipAddress>
<portNo>80</portNo>
<protocol>HTTP</protocol>
<macAddress>c4:2f:90:a7:b5:d1</macAddress>
<channelID>1</channelID>
<dateTime>2023-11-02T21:10:32+01:00</dateTime>
<activePostCount>1</activePostCount>
<eventType>ANPR</eventType>
<eventState>active</eventState>
<eventDescription>ANPR</eventDescription>
<channelName>Entrance</channelName>
<ANPR>
<country>19</country>
<licensePlate>ZG6140G</licensePlate>
<line>1</line>
<direction>forward</direction>
<confidenceLevel>100</confidenceLevel>
<plateType>unknown</plateType>
<plateColor>unknown</plateColor>
<licenseBright>0</licenseBright>
<dangmark>no</dangmark>
<twoWheelVehicle>no</twoWheelVehicle>
<threeWheelVehicle>no</threeWheelVehicle>
<plateCharBelieve>99,99,99,99,99,99,99</plateCharBelieve>
<vehicleType>vehicle</vehicleType>
<detectDir>8</detectDir>
<detectType>0</detectType>
<alarmDataType>0</alarmDataType>
<vehicleInfo>
<index>405</index>
<colorDepth>2</colorDepth>
<color>gray</color>
<length>0</length>
<vehicleLogoRecog>1134</vehicleLogoRecog>
<vehileSubLogoRecog>0</vehileSubLogoRecog>
<vehileModel>0</vehileModel>
</vehicleInfo>
<pictureInfoList>
<pictureInfo>
<fileName>licensePlatePicture.jpg</fileName>
<type>licensePlatePicture</type>
<dataType>0</dataType>
<absTime>20231102211032591</absTime>
<pId>2023110221103257100GG2XE4w2NmFEo</pId>
</pictureInfo>
<pictureInfo>
<fileName>vehiclePicture.jpg</fileName>
<type>vehiclePicture</type>
<dataType>0</dataType>
<absTime>20231102211032591</absTime>
<plateRect><X>448</X><Y>406</Y><width>117</width><height>40</height></plateRect>
<pId>2023110221103257200tVVeTdMnhzKm8</pId>
</pictureInfo>
</pictureInfoList>
<originalLicensePlate>ZG6140G</originalLicensePlate>
<CRIndex>19</CRIndex>
<vehicleListName>otherList</vehicleListName>
</ANPR>
<UUID>2023110221103259100tVVeTdMnhzKm8</UUID>
<picNum>2</picNum>
<monitoringSiteID></monitoringSiteID>
<isDataRetransmission>false</isDataRetransmission>
</EventNotificationAlert>
)";

const std::string kHeartbeat = R"(<?xml version="1.0" encoding="UTF-8"?>
<EventNotificationAlert version="2.0" xmlns="http://www.hikvision.com/ver20/XMLSchema">
<ipAddress>192.168.1.64</ipAddress>
<portNo>80</portNo>
<protocol>HTTP</protocol>
<macAddress>c4:2f:90:a7:b5:d1</macAddress>
<channelID>1</channelID>
<dateTime>2023-11-02T21:10:40+01:00</dateTime>
<activePostCount>0</activePostCount>
<eventType>videoloss</eventType>
<eventState>inactive</eventState>
<eventDescription>videoloss alarm</eventDescription>
</EventNotificationAlert>
)";

/// Every query on every prefix and on single-byte corruptions of `document`: nothing may throw,
/// crash or hang. Returns how many prefixes still yielded `name`'s complete text.
std::size_t exerciseTruncations(const std::string& document, const std::string& name) {
    std::size_t complete = 0;
    for (std::size_t length = 0; length <= document.size(); ++length) {
        const std::string prefix = document.substr(0, length);
        anpr::xml::rootName(prefix);
        anpr::xml::elements(prefix);
        anpr::xml::allTexts(prefix, name);
        anpr::xml::allElements(prefix, name);
        anpr::xml::decodeEntities(prefix);
        if (anpr::xml::firstText(prefix, name)) {
            ++complete;
        }
    }
    return complete;
}

/// A SADP field, or "<missing>".
std::string field(const anpr::hikvision::SadpDevice& device, const std::string& name) {
    const auto found = device.fields.find(name);
    return found == device.fields.end() ? std::string("<missing>") : found->second;
}

/// A part header, or "<missing>" (map::at would throw and abort the whole test run).
std::string header(const MultipartParser::Part& part, const std::string& name) {
    const auto found = part.headers.find(name);
    return found == part.headers.end() ? std::string("<missing>") : found->second;
}

struct StreamParts {
    std::vector<MultipartParser::Part> parts;
    std::size_t max_buffered{0};
};

StreamParts feedInChunks(const std::string& boundary, const std::string& stream,
                         const std::vector<std::size_t>& chunk_sizes) {
    StreamParts result;
    MultipartParser parser(boundary);
    std::size_t offset = 0;
    std::size_t chunk = 0;
    while (offset < stream.size()) {
        const std::size_t size = chunk_sizes[chunk++ % chunk_sizes.size()];
        parser.feed(stream.substr(offset, size));
        offset += size;
        MultipartParser::Part part;
        while (parser.next(part)) {
            result.parts.push_back(part);
            part = MultipartParser::Part{};
        }
        result.max_buffered = std::max(result.max_buffered, parser.buffered());
    }
    return result;
}

std::string fakeJpeg() {
    std::string jpeg("\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01", 12);
    // Bytes that look like a delimiter must not end a part whose Content-Length is known.
    jpeg += "\r\n--boundary\r\nContent-Type: text/plain\r\n\r\n";
    for (int i = 0; i < 700; ++i) {
        jpeg.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
    }
    jpeg += std::string("\x00\r\n\xFF\xD9", 5);
    return jpeg;
}

/// An alertStream body: preamble, ANPR XML with Content-Length, a JPEG with Content-Length and a
/// heartbeat without one, then the start of a part that never completes.
std::string alertStream(const std::string& newline) {
    const std::string jpeg = fakeJpeg();
    std::string stream = "HTTP preamble the parser must skip" + newline;
    stream += "--boundary" + newline;
    stream += "Content-Type: application/xml; charset=\"UTF-8\"" + newline;
    stream += "Content-Length: " + std::to_string(kAnprAlert.size()) + newline + newline;
    stream += kAnprAlert + newline;
    stream += "--boundary" + newline;
    stream += "Content-Disposition: form-data; name=\"licensePlatePicture.jpg\";" + newline;
    stream += "  filename=\"licensePlatePicture.jpg\"" + newline;
    stream += "Content-Type: image/jpeg" + newline;
    stream += "Content-Length: " + std::to_string(jpeg.size()) + newline + newline;
    stream += jpeg + newline;
    stream += "--boundary" + newline;
    stream += "Content-Type: application/xml" + newline + newline;
    stream += kHeartbeat + newline;
    stream += "--boundary" + newline;
    stream += "Content-Type: application/xml" + newline;
    return stream;
}

void checkAlertStreamParts(const std::vector<MultipartParser::Part>& parts) {
    CHECK_EQ(parts.size(), std::size_t{3});
    CHECK_EQ(parts[0].body, kAnprAlert);
    CHECK_EQ(header(parts[0], "content-type"),
             std::string("application/xml; charset=\"UTF-8\""));
    CHECK_EQ(parts[1].body, fakeJpeg());
    CHECK_EQ(header(parts[1], "content-type"), std::string("image/jpeg"));
    CHECK_EQ(header(parts[1], "content-disposition"),
             std::string("form-data; name=\"licensePlatePicture.jpg\"; "
                         "filename=\"licensePlatePicture.jpg\""));
    CHECK_EQ(parts[2].body, kHeartbeat);
    CHECK(parts[2].headers.count("content-length") == 0);
    CHECK(anpr::hikvision::parseAnprAlert(parts[0].body).has_value());
    CHECK(!anpr::hikvision::parseAnprAlert(parts[2].body).has_value());
}

}  // namespace

// ----------------------------------------------------------------------------- xml_lite

TEST("xml matches local names and ignores namespace prefixes") {
    const std::string soap =
        "<s:Envelope xmlns:s=\"x\"><s:Body><tds:GetDeviceInformationResponse>"
        "<tds:Manufacturer>HIKVISION</tds:Manufacturer><tds:Model>DS-TCG406-E</tds:Model>"
        "</tds:GetDeviceInformationResponse></s:Body></s:Envelope>";
    CHECK_EQ(anpr::xml::firstText(soap, "Model").value_or(""), std::string("DS-TCG406-E"));
    CHECK_EQ(anpr::xml::firstText(soap, "tds:Model").value_or(""), std::string("DS-TCG406-E"));
    CHECK_EQ(anpr::xml::firstText(soap, "Manufacturer").value_or(""), std::string("HIKVISION"));
    CHECK(!anpr::xml::firstText(soap, "model").has_value());  // names are case-sensitive
    CHECK(!anpr::xml::firstText(soap, "Mode").has_value());
    CHECK(!anpr::xml::firstText(soap, "").has_value());
    CHECK_EQ(anpr::xml::rootName(soap), std::string("Envelope"));
}

TEST("xml skips attributes even when a quoted value contains markup") {
    const std::string doc = "<r><a x=\"1>2\" y='</a>' z=\"/\">value</a><b k=\"v\"/></r>";
    CHECK_EQ(anpr::xml::firstText(doc, "a").value_or(""), std::string("value"));
    CHECK_EQ(anpr::xml::firstElement(doc, "b").value_or("x"), std::string());
}

TEST("xml skips comments, processing instructions and DOCTYPE") {
    const std::string doc =
        "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE r [ <!ENTITY e \"x>y\"> <!ELEMENT a (#PCDATA)> ]>\n"
        "<!-- <a>commented</a> -->\n"
        "<r><?pi <a>not</a> ?><a>yes<!-- no --></a></r>";
    CHECK_EQ(anpr::xml::rootName(doc), std::string("r"));
    CHECK_EQ(anpr::xml::firstText(doc, "a").value_or(""), std::string("yes"));
    CHECK_EQ(anpr::xml::allTexts(doc, "a").size(), std::size_t{1});
}

TEST("xml handles self-closing elements") {
    const std::string doc = "<r><a/><a >x</a><a k='1' /></r>";
    CHECK_EQ(anpr::xml::firstText(doc, "a").value_or("missing"), std::string());
    const auto texts = anpr::xml::allTexts(doc, "a");
    CHECK_EQ(texts.size(), std::size_t{3});
    CHECK_EQ(texts[1], std::string("x"));
    CHECK_EQ(texts[2], std::string());
    CHECK_EQ(anpr::xml::rootName("<only/>"), std::string("only"));
}

TEST("xml pairs nested same-name elements with their own close tags") {
    const std::string doc = "<r><b><x/><b>in</b>tail</b><b>2</b></r>";
    CHECK_EQ(anpr::xml::firstElement(doc, "b").value_or(""), std::string("<x/><b>in</b>tail"));
    CHECK_EQ(anpr::xml::firstText(doc, "b").value_or(""), std::string("intail"));
    const auto blocks = anpr::xml::allElements(doc, "b");
    CHECK_EQ(blocks.size(), std::size_t{2});
    CHECK_EQ(blocks[1], std::string("2"));
    // Prefixes differ, local names match: still paired correctly.
    const std::string prefixed = "<p:B><q:B>1</q:B></p:B>";
    CHECK_EQ(anpr::xml::firstElement(prefixed, "B").value_or(""), std::string("<q:B>1</q:B>"));
}

TEST("xml decodes CDATA verbatim and entities inside text") {
    const std::string doc = "<a>x &amp; <![CDATA[<b>&amp;]]]]> y</a>";
    CHECK_EQ(anpr::xml::firstText(doc, "a").value_or(""), std::string("x & <b>&amp;]] y"));
    const auto elements = anpr::xml::elements("<r><a><![CDATA[1<2]]></a></r>");
    CHECK_EQ(elements.size(), std::size_t{2});
    CHECK_EQ(elements[1].text, std::string("1<2"));
}

TEST("xml decodes predefined entities and numeric references to UTF-8") {
    CHECK_EQ(anpr::xml::decodeEntities("&amp;&lt;&gt;&quot;&apos;"), std::string("&<>\"'"));
    CHECK_EQ(anpr::xml::decodeEntities("&#65;&#x42;&#X43;&#x00044;"), std::string("ABCD"));
    CHECK_EQ(anpr::xml::decodeEntities("&#1179;"), std::string("\xD2\x9B"));      // қ
    CHECK_EQ(anpr::xml::decodeEntities("&#x4E2D;"), std::string("\xE4\xB8\xAD"));  // 中
    CHECK_EQ(anpr::xml::decodeEntities("&#128512;"), std::string("\xF0\x9F\x98\x80"));
    // Unknown, invalid or unterminated references are kept as written.
    CHECK_EQ(anpr::xml::decodeEntities("&nbsp; & &#; &#x; &#0; &#xD800; &#x110000; &amp"),
             std::string("&nbsp; & &#; &#x; &#0; &#xD800; &#x110000; &amp"));
    CHECK_EQ(anpr::xml::decodeEntities("a&&lt;b"), std::string("a&<b"));
    CHECK_EQ(anpr::xml::decodeEntities("&#99999999999999999999;"),
             std::string("&#99999999999999999999;"));
}

TEST("xml escape round-trips through decodeEntities") {
    const std::string text = "<a href=\"x\">Tom & Jerry's</a>";
    const std::string escaped = anpr::xml::escape(text);
    CHECK_EQ(escaped,
             std::string("&lt;a href=&quot;x&quot;&gt;Tom &amp; Jerry&apos;s&lt;/a&gt;"));
    CHECK_EQ(anpr::xml::decodeEntities(escaped), text);
    CHECK_EQ(anpr::xml::firstText("<v>" + escaped + "</v>", "v").value_or(""), text);
}

TEST("xml trim removes ASCII whitespace only at the ends") {
    CHECK_EQ(anpr::xml::trim(" \t\r\n a b \n"), std::string("a b"));
    CHECK_EQ(anpr::xml::trim("   "), std::string());
    CHECK_EQ(anpr::xml::trim(""), std::string());
    CHECK_EQ(anpr::xml::trim("x"), std::string("x"));
}

TEST("xml rootName skips junk before the root") {
    CHECK_EQ(anpr::xml::rootName(std::string("\x01\x02\x00\x7f", 4) + "<ProbeMatch/>"),
             std::string("ProbeMatch"));
    CHECK_EQ(anpr::xml::rootName("<?xml version=\"1.0\"?><!-- c --><s:Envelope>"),
             std::string("Envelope"));
    CHECK_EQ(anpr::xml::rootName(""), std::string());
    CHECK_EQ(anpr::xml::rootName("plain text"), std::string());
    CHECK_EQ(anpr::xml::rootName("<!-- never closed <a>"), std::string());
}

TEST("xml elements walk reports depth, own text, children and closure") {
    const auto elements =
        anpr::xml::elements("<r>head<a>1</a><b><c>2</c>tail</b><d>open");
    CHECK_EQ(elements.size(), std::size_t{5});
    CHECK_EQ(elements[0].name, std::string("r"));
    CHECK_EQ(elements[0].depth, 0);
    CHECK(elements[0].has_children);
    CHECK(!elements[0].closed);
    CHECK_EQ(elements[0].text, std::string("head"));
    CHECK_EQ(elements[1].name, std::string("a"));
    CHECK_EQ(elements[1].depth, 1);
    CHECK(elements[1].closed);
    CHECK(!elements[1].has_children);
    CHECK_EQ(elements[2].text, std::string("tail"));
    CHECK_EQ(elements[3].depth, 2);
    CHECK_EQ(elements[3].text, std::string("2"));
    CHECK_EQ(elements[4].name, std::string("d"));
    CHECK(!elements[4].closed);
    // A mismatched close tag closes the enclosing element it names.
    const auto recovered = anpr::xml::elements("<r><a><b>x</a><c/></r>");
    CHECK_EQ(recovered.size(), std::size_t{4});
    CHECK(recovered[1].closed);
    CHECK(!recovered[2].closed);
    CHECK_EQ(recovered[3].depth, 1);
    CHECK(recovered[0].closed);
}

TEST("xml truncated and malformed input yields nullopt, never a crash") {
    CHECK(!anpr::xml::firstText("<a>text", "a").has_value());
    CHECK(!anpr::xml::firstText("<a>te</a", "a").has_value());
    CHECK(!anpr::xml::firstText("<a x=\"unterminated>v</a>", "a").has_value());
    CHECK(!anpr::xml::firstText("<!-- open <a>x</a>", "a").has_value());
    CHECK(!anpr::xml::firstText("<a><![CDATA[x</a>", "a").has_value());
    CHECK(!anpr::xml::firstText("<", "a").has_value());
    CHECK(!anpr::xml::firstText("</a>", "a").has_value());
    CHECK_EQ(anpr::xml::firstText("<a>1 < 2</a>", "a").value_or(""), std::string("1 < 2"));
    CHECK(anpr::xml::allTexts("<a>1</a><a>2", "a").size() == 1);

    // Only prefixes that include </model> yield the model.
    const std::size_t complete = exerciseTruncations(kDeviceInfo, "model");
    const std::size_t close = kDeviceInfo.find("</model>") + std::string("</model>").size();
    CHECK_EQ(complete, kDeviceInfo.size() + 1 - close);
    exerciseTruncations(kAnprAlert, "licensePlate");
    exerciseTruncations(kWsProbeMatches, "Address");
}

TEST("xml survives random markup-like garbage") {
    const std::string alphabet = "<>/!?-[]CDATA=\"' a:b&#x;0\n";
    std::mt19937 random(20261009U);
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::uniform_int_distribution<std::size_t> length(0, 200);
    for (int round = 0; round < 3000; ++round) {
        std::string text;
        const std::size_t size = length(random);
        for (std::size_t i = 0; i < size; ++i) {
            text.push_back(alphabet[pick(random)]);
        }
        anpr::xml::rootName(text);
        anpr::xml::elements(text);
        anpr::xml::firstText(text, "a");
        anpr::xml::allElements(text, "b");
        anpr::xml::decodeEntities(text);
    }
    CHECK(true);
}

TEST("xml deep nesting stays linear and needs no recursion") {
    const std::size_t depth = 50000;
    std::string doc;
    for (std::size_t i = 0; i < depth; ++i) {
        doc += "<a>";
    }
    doc += "x";
    for (std::size_t i = 0; i < depth; ++i) {
        doc += "</a>";
    }
    CHECK_EQ(anpr::xml::firstText(doc, "a").value_or(""), std::string("x"));
    CHECK_EQ(anpr::xml::allElements(doc, "a").size(), std::size_t{1});
    const auto elements = anpr::xml::elements(doc);
    CHECK_EQ(elements.size(), depth);
    CHECK(elements.back().closed);
    // A flood of stray close tags under deep nesting is also cheap.
    std::string stray = doc.substr(0, depth * 3);
    for (std::size_t i = 0; i < depth; ++i) {
        stray += "</z>";
    }
    CHECK_EQ(anpr::xml::elements(stray).size(), depth);
}

// ----------------------------------------------------------------------------- SADP

TEST("sadp builds the inquiry verbatim with an upper-case uuid") {
    CHECK_EQ(anpr::hikvision::buildSadpProbe("4c2d1f3a-7b8e-4d2c-9a61-3e5f7b9c1d2e"),
             std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?><Probe>"
                         "<Uuid>4C2D1F3A-7B8E-4D2C-9A61-3E5F7B9C1D2E</Uuid>"
                         "<Types>inquiry</Types></Probe>"));
}

TEST("sadp parses a ProbeMatch behind a binary prefix") {
    const std::string payload = std::string("\x21\x02\x00\x9c", 4) + kProbeMatch;
    const auto device = anpr::hikvision::parseSadpResponse(payload, ip("192.168.1.64"));
    CHECK(device.has_value());
    CHECK(device->from == ip("192.168.1.64"));
    CHECK_EQ(device->uuid, std::string("4C2D1F3A-7B8E-4D2C-9A61-3E5F7B9C1D2E"));
    CHECK_EQ(device->device_type, std::string("139267"));
    CHECK_EQ(device->model, std::string("DS-TCG406-E"));
    CHECK_EQ(device->serial, std::string("DS-TCG406-E20240512AAWRFA1234567"));
    CHECK_EQ(device->mac, std::string("c4:2f:90:a7:b5:d1"));
    CHECK_EQ(device->ipv4, std::string("192.168.1.64"));
    CHECK_EQ(device->subnet_mask, std::string("255.255.255.0"));
    CHECK_EQ(device->gateway, std::string("192.168.1.1"));
    CHECK(device->dhcp);
    CHECK_EQ(device->http_port, std::uint16_t{80});
    CHECK_EQ(device->command_port, std::uint16_t{8000});
    CHECK_EQ(device->software_version, std::string("V5.6.10build 230721"));
    CHECK_EQ(device->boot_time, std::string("2026-10-08 07:12:03"));
    CHECK(device->activated.has_value());
    CHECK(!*device->activated);
    CHECK_EQ(device->fields.size(), std::size_t{29});
    CHECK_EQ(field(*device, "DSPVersion"), std::string("V7.3 build 230721"));
    CHECK_EQ(field(*device, "SDKOverTLSPort"), std::string("8443"));
    CHECK_EQ(field(*device, "OEMInfo"), std::string("N/A"));
}

TEST("sadp parses an activated static device without a declaration") {
    std::string xml = kProbeMatch.substr(kProbeMatch.find("<ProbeMatch>"));
    xml.replace(xml.find("<DHCP>true"), 10, "<DHCP>false");
    xml.replace(xml.find("<Activated>false"), 16, "<Activated>true");
    const auto device = anpr::hikvision::parseSadpResponse(xml, ip("192.168.10.21"));
    CHECK(device.has_value());
    CHECK(!device->dhcp);
    CHECK(device->activated.value_or(false));
}

TEST("sadp rejects our own looped-back inquiry and non-answers") {
    const auto from = ip("192.168.10.2");
    CHECK(!anpr::hikvision::parseSadpResponse(
               anpr::hikvision::buildSadpProbe("4C2D1F3A-7B8E-4D2C-9A61-3E5F7B9C1D2E"), from)
               .has_value());
    CHECK(!anpr::hikvision::parseSadpResponse("", from).has_value());
    CHECK(!anpr::hikvision::parseSadpResponse("M-SEARCH * HTTP/1.1\r\n\r\n", from).has_value());
    CHECK(!anpr::hikvision::parseSadpResponse("<?xml version=\"1.0\"?><Hello/>", from)
               .has_value());
    // Neither MAC nor IPv4 address: nothing to identify or reach.
    CHECK(!anpr::hikvision::parseSadpResponse(
               "<ProbeMatch><DeviceDescription>DS-TCG406-E</DeviceDescription></ProbeMatch>", from)
               .has_value());
    // A datagram cut short is refused rather than reported with missing fields.
    CHECK(!anpr::hikvision::parseSadpResponse(kProbeMatch.substr(0, kProbeMatch.size() / 2), from)
               .has_value());
}

TEST("sadp tolerates missing and garbled optional fields") {
    const auto device = anpr::hikvision::parseSadpResponse(
        "<ProbeMatch><IPv4Address> 192.168.1.64 </IPv4Address><HttpPort>70000</HttpPort>"
        "<CommandPort>80a</CommandPort><MAC>not-a-mac</MAC><DHCP>maybe</DHCP></ProbeMatch>",
        ip("192.168.1.64"));
    CHECK(device.has_value());
    CHECK_EQ(device->ipv4, std::string("192.168.1.64"));
    CHECK_EQ(device->http_port, std::uint16_t{0});
    CHECK_EQ(device->command_port, std::uint16_t{0});
    CHECK(device->mac.empty());
    CHECK(!device->dhcp);
    CHECK(!device->activated.has_value());
}

// ----------------------------------------------------------------------------- ONVIF

TEST("onvif builds a WS-Discovery probe for NetworkVideoTransmitter") {
    const std::string probe =
        anpr::hikvision::buildWsDiscoveryProbe("0b6f5a1e-2c3d-4e5f-8a9b-0c1d2e3f4a5b");
    CHECK_EQ(anpr::xml::rootName(probe), std::string("Envelope"));
    CHECK(probe.find("xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"") != std::string::npos);
    CHECK(probe.find("xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"") !=
          std::string::npos);
    CHECK(probe.find("xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"") !=
          std::string::npos);
    CHECK(probe.find("xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"") != std::string::npos);
    CHECK_EQ(anpr::xml::firstText(probe, "Action").value_or(""),
             std::string("http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe"));
    CHECK_EQ(anpr::xml::firstText(probe, "MessageID").value_or(""),
             std::string("uuid:0b6f5a1e-2c3d-4e5f-8a9b-0c1d2e3f4a5b"));
    CHECK_EQ(anpr::xml::firstText(probe, "To").value_or(""),
             std::string("urn:schemas-xmlsoap-org:ws:2005:04:discovery"));
    CHECK_EQ(anpr::xml::firstText(anpr::xml::firstElement(probe, "Probe").value_or(""), "Types")
                 .value_or(""),
             std::string("dn:NetworkVideoTransmitter"));
    // A caller passing a prefixed id does not get it twice.
    CHECK_EQ(anpr::xml::firstText(anpr::hikvision::buildWsDiscoveryProbe("urn:uuid:abc"),
                                  "MessageID")
                 .value_or(""),
             std::string("uuid:abc"));
    // Our own probe looped back is not a match.
    CHECK(anpr::hikvision::parseProbeMatches(probe, ip("192.168.1.2")).empty());
}

TEST("onvif parses a Hikvision ProbeMatch with scopes, MAC and IPv4 XAddr") {
    const auto matches = anpr::hikvision::parseProbeMatches(kWsProbeMatches, ip("192.168.1.64"));
    CHECK_EQ(matches.size(), std::size_t{1});
    const auto& match = matches[0];
    CHECK(match.from == ip("192.168.1.64"));
    CHECK_EQ(match.endpoint, std::string("urn:uuid:3fa1fe68-b915-4053-a3e1-c42f90a7b5d1"));
    CHECK_EQ(match.types, std::string("dn:NetworkVideoTransmitter tds:Device"));
    CHECK_EQ(match.scopes.size(), std::size_t{7});
    CHECK_EQ(match.scopes[4], std::string("onvif://www.onvif.org/hardware/DS-TCG406-E"));
    CHECK_EQ(match.hardware, std::string("DS-TCG406-E"));
    CHECK_EQ(match.name, std::string("HIKVISION DS-TCG406-E"));
    CHECK_EQ(match.location, std::string("city/hangzhou"));
    CHECK_EQ(match.mac, std::string("c4:2f:90:a7:b5:d1"));
    CHECK_EQ(match.xaddrs.size(), std::size_t{2});
    // The IPv6 XAddr comes first and is skipped.
    CHECK(match.xaddr_host == ip("192.168.1.64"));
    CHECK_EQ(match.xaddr_port, std::uint16_t{80});
    CHECK_EQ(match.xaddr_path, std::string("/onvif/device_service"));
}

TEST("onvif takes the MAC from a scope and leaves non-Hikvision UUIDs alone") {
    const std::string payload =
        "<Envelope><Body><ProbeMatches>"
        "<ProbeMatch><EndpointReference><Address>urn:uuid:2419d68a-2dd2-21b2-a205-ec71dbd3a8e2"
        "</Address></EndpointReference><Scopes>onvif://www.onvif.org/MAC/00%3A11%3A22%3A33%3A44"
        "%3A55 onvif://www.onvif.org/Name/Gate%zz</Scopes>"
        "<XAddrs>http://cam.local/onvif http://10.0.0.5:8080/onvif/device_service</XAddrs>"
        "</ProbeMatch>"
        "<ProbeMatch><EndpointReference><Address>urn:uuid:2419d68a-2dd2-21b2-a205-ec71dbd3a8e2"
        "</Address></EndpointReference><XAddrs>http://10.0.0.6</XAddrs></ProbeMatch>"
        "<ProbeMatch><Types>dn:NetworkVideoTransmitter</Types></ProbeMatch>"
        "</ProbeMatches></Body></Envelope>";
    const auto matches = anpr::hikvision::parseProbeMatches(payload, ip("10.0.0.5"));
    CHECK_EQ(matches.size(), std::size_t{2});  // the third has neither endpoint nor XAddr
    CHECK_EQ(matches[0].mac, std::string("00:11:22:33:44:55"));
    CHECK_EQ(matches[0].name, std::string("Gate%zz"));
    CHECK(matches[0].xaddr_host == ip("10.0.0.5"));
    CHECK_EQ(matches[0].xaddr_port, std::uint16_t{8080});
    // ec:71:db is not a Hikvision OUI: the UUID tail is not trusted as a MAC.
    CHECK(matches[1].mac.empty());
    CHECK(matches[1].xaddr_host == ip("10.0.0.6"));
    CHECK_EQ(matches[1].xaddr_path, std::string("/"));
    CHECK(anpr::hikvision::parseProbeMatches("", ip("10.0.0.5")).empty());
    // Cut just before </ProbeMatch>: nothing is reported from the unfinished match.
    const std::string cut = kWsProbeMatches.substr(0, kWsProbeMatches.find("</wsadis:ProbeMatch>"));
    CHECK(anpr::hikvision::parseProbeMatches(cut, ip("10.0.0.5")).empty());
}

TEST("onvif state names") {
    CHECK_EQ(anpr::hikvision::toString(anpr::hikvision::OnvifState::kAvailable),
             std::string("available"));
    CHECK_EQ(anpr::hikvision::toString(anpr::hikvision::OnvifState::kAuthRequired),
             std::string("auth_required"));
    CHECK_EQ(anpr::hikvision::toString(anpr::hikvision::OnvifState::kUnavailable),
             std::string("unavailable"));
    CHECK_EQ(anpr::hikvision::toString(anpr::hikvision::OnvifState::kError),
             std::string("error"));
}

// ----------------------------------------------------------------------------- ISAPI

TEST("isapi parses deviceInfo") {
    const auto info = anpr::hikvision::parseDeviceInfo(kDeviceInfo);
    CHECK(info.ok);
    CHECK_EQ(info.device_name, std::string("Entrance"));
    CHECK_EQ(info.device_id, std::string("6aff4000-1000-11b2-8000-c42f90a7b5d1"));
    CHECK_EQ(info.model, std::string("DS-TCG406-E"));
    CHECK_EQ(info.serial, std::string("DS-TCG406-E20240512AAWRFA1234567"));
    CHECK_EQ(info.mac, std::string("c4:2f:90:a7:b5:d1"));
    CHECK_EQ(info.firmware, std::string("V5.7.18"));
    CHECK_EQ(info.firmware_date, std::string("build 240826"));
    CHECK_EQ(info.device_type, std::string("IPCamera"));
    CHECK_EQ(info.hardware_version, std::string("0x0"));

    const auto error = anpr::hikvision::parseDeviceInfo(
        "<ResponseStatus><statusCode>4</statusCode><statusString>Invalid Operation"
        "</statusString></ResponseStatus>");
    CHECK(!error.ok);
    CHECK(!anpr::hikvision::parseDeviceInfo("<html><body>401</body></html>").ok);
}

TEST("isapi parses the streaming channel video block") {
    const auto stream = anpr::hikvision::parseStreamingChannel(kStreamingChannel);
    CHECK(stream.ok);
    CHECK_EQ(stream.codec, std::string("H.265"));
    CHECK_EQ(stream.width, 2688);
    CHECK_EQ(stream.height, 1520);
    CHECK_NEAR(stream.max_fps, 25.0, 1e-9);
    CHECK_EQ(stream.bitrate_control, std::string("VBR"));

    const auto fractional = anpr::hikvision::parseStreamingChannel(
        "<StreamingChannel><Video><videoCodecType>H.264</videoCodecType>"
        "<maxFrameRate>1250</maxFrameRate><videoResolutionWidth>x</videoResolutionWidth>"
        "</Video></StreamingChannel>");
    CHECK(fractional.ok);
    CHECK_NEAR(fractional.max_fps, 12.5, 1e-9);
    CHECK_EQ(fractional.width, 0);
    CHECK(!anpr::hikvision::parseStreamingChannel("<StreamingChannel/>").ok);
}

TEST("isapi detects native ANPR from capability documents") {
    using anpr::hikvision::parseNativeAnprSupport;
    const std::string traffic =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<TrafficCap version=\"2.0\" xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
        "<isSupportPlateRecognitionParam>true</isSupportPlateRecognitionParam></TrafficCap>";
    const std::string event_flag =
        "<EventCap><isSupportMotionDetection>true</isSupportMotionDetection>"
        "<isSupportVehicleDetection>true</isSupportVehicleDetection></EventCap>";
    const std::string event_block =
        "<EventCap><ANPR><isSupportPicture>true</isSupportPicture></ANPR></EventCap>";
    const std::string event_list = "<EventCap><eventTypes>VMD,ANPR,tamper</eventTypes></EventCap>";
    const std::string generic =
        "<DeviceCap><SysCap><isSupportDst>true</isSupportDst></SysCap>"
        "<isSupportVehicleDetection>false</isSupportVehicleDetection></DeviceCap>";
    const std::string not_supported =
        "<ResponseStatus version=\"2.0\"><requestURL>/ISAPI/Traffic/capabilities</requestURL>"
        "<statusCode>4</statusCode><statusString>Invalid Operation</statusString>"
        "<subStatusCode>notSupport</subStatusCode></ResponseStatus>";
    const std::string unauthorized =
        "<ResponseStatus><statusCode>4</statusCode><statusString>Invalid Operation"
        "</statusString><subStatusCode>badAuthorization</subStatusCode></ResponseStatus>";

    CHECK(parseNativeAnprSupport({traffic}) == NativeAnprSupport::kSupported);
    CHECK(parseNativeAnprSupport({"<ITCCap/>"}) == NativeAnprSupport::kSupported);
    CHECK(parseNativeAnprSupport({generic, event_flag}) == NativeAnprSupport::kSupported);
    CHECK(parseNativeAnprSupport({event_block}) == NativeAnprSupport::kSupported);
    CHECK(parseNativeAnprSupport({event_list}) == NativeAnprSupport::kSupported);
    CHECK(parseNativeAnprSupport({generic}) == NativeAnprSupport::kNotSupported);
    CHECK(parseNativeAnprSupport({not_supported, generic}) == NativeAnprSupport::kNotSupported);
    CHECK(parseNativeAnprSupport({not_supported}) == NativeAnprSupport::kNotSupported);
    CHECK(parseNativeAnprSupport({}) == NativeAnprSupport::kUnknown);
    CHECK(parseNativeAnprSupport({"", "garbage", "<html><body>404</body></html>"}) ==
          NativeAnprSupport::kUnknown);
    CHECK(parseNativeAnprSupport({unauthorized}) == NativeAnprSupport::kUnknown);
    // "<SwitchCap>" must not be read as ITC.
    CHECK(parseNativeAnprSupport({"<SwitchCap><x>1</x></SwitchCap>"}) ==
          NativeAnprSupport::kNotSupported);
    CHECK_EQ(anpr::hikvision::toString(NativeAnprSupport::kSupported), std::string("supported"));
    CHECK_EQ(anpr::hikvision::toString(NativeAnprSupport::kNotSupported),
             std::string("not_supported"));
    CHECK_EQ(anpr::hikvision::toString(NativeAnprSupport::kUnknown), std::string("unknown"));
}

TEST("isapi parses the captured ANPR alert") {
    const auto event = anpr::hikvision::parseAnprAlert(kAnprAlert);
    CHECK(event.has_value());
    CHECK_EQ(event->plate, std::string("ZG6140G"));
    CHECK_EQ(event->country, std::string("19"));
    CHECK_EQ(event->direction, std::string("forward"));
    CHECK_NEAR(event->confidence, 100.0, 1e-9);
    CHECK_EQ(event->plate_color, std::string("unknown"));
    CHECK_EQ(event->vehicle_type, std::string("vehicle"));
    CHECK_EQ(event->lane, std::string("1"));
    CHECK_EQ(event->camera_time, std::string("2023-11-02T21:10:32+01:00"));
    CHECK_EQ(event->channel, std::string("1"));
    CHECK(event->camera_id.empty());
    CHECK_EQ(event->unix_time_ms, std::int64_t{0});
}

TEST("isapi ANPR alert variants and rejections") {
    using anpr::hikvision::parseAnprAlert;
    CHECK(!parseAnprAlert(kHeartbeat).has_value());
    CHECK(!parseAnprAlert("").has_value());
    CHECK(!parseAnprAlert(kAnprAlert.substr(0, kAnprAlert.size() / 3)).has_value());
    CHECK(!parseAnprAlert("<ResponseStatus><ANPR><licensePlate>X</licensePlate></ANPR>"
                          "</ResponseStatus>")
               .has_value());

    // Lower-case event type, no ANPR block, laneNo and dynChannelID, KZ country code.
    const auto flat = parseAnprAlert(
        "<EventNotificationAlert><dynChannelID>2</dynChannelID><eventType>anpr</eventType>"
        "<licensePlate> 123ABC02 </licensePlate><country>30</country><laneNo>2</laneNo>"
        "<confidenceLevel>87.5</confidenceLevel></EventNotificationAlert>");
    CHECK(flat.has_value());
    CHECK_EQ(flat->plate, std::string("123ABC02"));
    CHECK_EQ(flat->lane, std::string("2"));
    CHECK_EQ(flat->channel, std::string("2"));
    CHECK_NEAR(flat->confidence, 87.5, 1e-9);
    CHECK_EQ(anpr::hikvision::anprCountryIso(flat->country), std::string("KZ"));

    // An unread plate falls back to originalLicensePlate; "unknown" everywhere is no event.
    std::string fallback = kAnprAlert;
    fallback.replace(fallback.find("ZG6140G"), 7, "unknown");
    const auto original = parseAnprAlert(fallback);
    CHECK(original.has_value());
    CHECK_EQ(original->plate, std::string("ZG6140G"));
    fallback.replace(fallback.find("ZG6140G"), 7, "");
    CHECK(!parseAnprAlert(fallback).has_value());

    // Confidence absent stays negative.
    const auto bare = parseAnprAlert(
        "<EventNotificationAlert><eventType>VMD</eventType><ANPR><licensePlate>777KZ01"
        "</licensePlate></ANPR></EventNotificationAlert>");
    CHECK(bare.has_value());
    CHECK(bare->confidence < 0.0);
    CHECK(!parseAnprAlert("<EventNotificationAlert><eventType>VMD</eventType>"
                          "<licensePlate>777KZ01</licensePlate></EventNotificationAlert>")
               .has_value());
}

TEST("isapi maps ANPR country codes") {
    using anpr::hikvision::anprCountryIso;
    CHECK_EQ(anprCountryIso("30"), std::string("KZ"));
    CHECK_EQ(anprCountryIso(" 11 "), std::string("RU"));
    CHECK_EQ(anprCountryIso("33"), std::string("UZ"));
    CHECK_EQ(anprCountryIso("73"), std::string("KG"));
    CHECK_EQ(anprCountryIso("19"), std::string("HR"));
    CHECK_EQ(anprCountryIso("kz"), std::string("KZ"));
    CHECK(anprCountryIso("255").empty());
    CHECK(anprCountryIso("0").empty());
    CHECK(anprCountryIso("NON").empty());
    CHECK(anprCountryIso("").empty());
}

TEST("isapi status names") {
    using anpr::hikvision::IsapiStatus;
    CHECK_EQ(anpr::hikvision::toString(IsapiStatus::kOk), std::string("ok"));
    CHECK_EQ(anpr::hikvision::toString(IsapiStatus::kAuthFailed), std::string("auth_failed"));
    CHECK_EQ(anpr::hikvision::toString(IsapiStatus::kUnavailable), std::string("unavailable"));
    CHECK_EQ(anpr::hikvision::toString(IsapiStatus::kError), std::string("error"));
    CHECK_EQ(anpr::hikvision::toString(IsapiStatus::kSkipped), std::string("skipped"));
}

TEST("multipart boundary from Content-Type") {
    using anpr::hikvision::boundaryFromContentType;
    CHECK_EQ(boundaryFromContentType("multipart/mixed; boundary=boundary"),
             std::string("boundary"));
    CHECK_EQ(boundaryFromContentType("multipart/mixed;boundary=\"MIME_boundary\""),
             std::string("MIME_boundary"));
    CHECK_EQ(boundaryFromContentType("multipart/form-data; charset=\"a;b\"; BOUNDARY = \"x;y\""),
             std::string("x;y"));
    CHECK_EQ(boundaryFromContentType("multipart/mixed; boundary=abc ; charset=utf-8"),
             std::string("abc"));
    CHECK_EQ(boundaryFromContentType(
                 "multipart/form-data; boundary=---------------------------7e13971310878"),
             std::string("---------------------------7e13971310878"));
    CHECK_EQ(boundaryFromContentType("multipart/mixed; myboundary=no"), std::string());
    CHECK_EQ(boundaryFromContentType("multipart/mixed"), std::string());
    CHECK_EQ(boundaryFromContentType("multipart/mixed; boundary"), std::string());
    CHECK_EQ(boundaryFromContentType(""), std::string());
}

TEST("multipart parses an alert stream fed in one piece and byte by byte") {
    const std::string stream = alertStream("\r\n");
    checkAlertStreamParts(feedInChunks("boundary", stream, {stream.size()}).parts);
    const auto byte_by_byte = feedInChunks("boundary", stream, {1});
    checkAlertStreamParts(byte_by_byte.parts);
    // Never more than the largest part plus its headers is held.
    CHECK(byte_by_byte.max_buffered < std::max(kAnprAlert.size(), fakeJpeg().size()) + 300);
}

TEST("multipart parses an alert stream split at random points") {
    const std::string stream = alertStream("\r\n");
    std::mt19937 random(37020U);
    std::uniform_int_distribution<std::size_t> size(1, 97);
    for (int round = 0; round < 200; ++round) {
        std::vector<std::size_t> chunks;
        for (int i = 0; i < 64; ++i) {
            chunks.push_back(size(random));
        }
        checkAlertStreamParts(feedInChunks("boundary", stream, chunks).parts);
    }
}

TEST("multipart accepts LF-only line endings") {
    const std::string stream = alertStream("\n");
    checkAlertStreamParts(feedInChunks("boundary", stream, {1}).parts);
    checkAlertStreamParts(feedInChunks("boundary", stream, {7, 3, 11}).parts);
}

TEST("multipart handles empty bodies, padding, close delimiters and epilogue") {
    MultipartParser parser("b");
    parser.feed("--b \t\r\nX-A: 1\r\n\r\n\r\n--b\r\n\r\npayload\r\n--b--\r\nepilogue\r\n"
                "--b\r\nX-B: 2\r\n\r\nlast\r\n--b--");
    std::vector<MultipartParser::Part> parts;
    MultipartParser::Part part;
    while (parser.next(part)) {
        parts.push_back(part);
    }
    CHECK_EQ(parts.size(), std::size_t{3});
    CHECK_EQ(parts[0].body, std::string());
    CHECK_EQ(header(parts[0], "x-a"), std::string("1"));
    CHECK_EQ(parts[1].body, std::string("payload"));
    CHECK(parts[1].headers.empty());
    CHECK_EQ(parts[2].body, std::string("last"));
    CHECK_EQ(header(parts[2], "x-b"), std::string("2"));
}

TEST("multipart ignores boundary text that is not a delimiter line") {
    MultipartParser parser("b");
    parser.feed("--b\r\n\r\nx--b y\r\n--bz\r\n--b-\r\nend\r\n--b\r\n");
    MultipartParser::Part part;
    CHECK(parser.next(part));
    CHECK_EQ(part.body, std::string("x--b y\r\n--bz\r\n--b-\r\nend"));
}

TEST("multipart accepts a boundary declared with its leading dashes") {
    // Some cameras declare boundary=--myboundary and then write "--myboundary" lines.
    const std::string body = "--myboundary\r\nA: 1\r\n\r\none\r\n----myboundary\r\nA: 2\r\n\r\n"
                             "two\r\n--myboundary\r\n";
    const auto parsed = feedInChunks("--myboundary", body, {5});
    CHECK_EQ(parsed.parts.size(), std::size_t{2});
    CHECK_EQ(parsed.parts[0].body, std::string("one"));
    CHECK_EQ(parsed.parts[1].body, std::string("two"));
}

TEST("multipart drops a part that never ends its headers") {
    MultipartParser parser("b");
    parser.feed("--b\r\n<xml>no headers</xml>\r\n--b\r\nA: 1\r\n\r\nok\r\n--b\r\n");
    MultipartParser::Part part;
    CHECK(parser.next(part));
    CHECK_EQ(part.body, std::string("ok"));
    CHECK_EQ(header(part, "a"), std::string("1"));
}

TEST("multipart keeps garbage bounded and reports what it holds") {
    MultipartParser parser("boundary");
    std::mt19937 random(8000U);
    std::uniform_int_distribution<int> byte(0, 255);
    MultipartParser::Part part;
    for (int round = 0; round < 1000; ++round) {
        std::string chunk;
        for (int i = 0; i < 1024; ++i) {
            const char ch = static_cast<char>(byte(random));
            chunk.push_back(ch == 'b' ? 'c' : ch);  // no boundary text in the garbage
        }
        parser.feed(chunk);
        CHECK(parser.buffered() <= std::string("--boundary").size());
        CHECK(!parser.next(part));
    }
    // A part whose headers never end is held, and buffered() says so.
    parser.feed("\r\n--boundary\r\n");
    for (int round = 0; round < 100; ++round) {
        parser.feed("X-Junk: " + std::string(1000, 'j') + "\r\n");
    }
    CHECK(!parser.next(part));
    CHECK(parser.buffered() > 100000);
    // Content-Length larger than what arrived: waits, and the bytes are counted.
    MultipartParser waiting("b");
    waiting.feed("--b\r\nContent-Length: 5000\r\n\r\n" + std::string(4000, 'x'));
    CHECK(!waiting.next(part));
    CHECK(waiting.buffered() > 4000);
    waiting.feed(std::string(1000, 'y'));
    CHECK(waiting.next(part));
    CHECK_EQ(part.body.size(), std::size_t{5000});
    CHECK_EQ(waiting.buffered(), std::size_t{0});
}

TEST("multipart with an empty boundary never produces parts") {
    MultipartParser parser("");
    parser.feed("--\r\n\r\nbody\r\n--\r\n");
    MultipartParser::Part part;
    CHECK(!parser.next(part));
}

// ---------------------------------------------------------------------------------------------
// Network: SADP answer merging and packet capture, discovery without an interface, ONVIF and
// ISAPI against scripted servers on 127.0.0.1, the native ANPR listener and its event JSON.
// ---------------------------------------------------------------------------------------------

namespace {

using anpr::hikvision::IsapiStatus;
using anpr::hikvision::NativeAnprListener;
using anpr::hikvision::NativePlateEvent;
using anpr::hikvision::OnvifState;
using anpr::net::Credentials;

const Ipv4 kLocalhost{0x7F000001U};
const Credentials kCameraLogin{"admin", "Kz#Secret:9"};
const Credentials kWrongLogin{"admin", "Kz#Secret:8"};
const Credentials kOnvifLogin{"onvif-user", "On<v>if&Pass\"1"};
const std::string kDigestNonce = "4e5449334d6a59334e5755364e7a6b305a6a5a6b5a6d4d3d";

using SteadyClock = std::chrono::steady_clock;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

double elapsedMs(SteadyClock::time_point start) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
}

void pauseMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// Polls `condition` every 10 ms for at most `timeout_ms`.
bool waitFor(const std::function<bool()>& condition, int timeout_ms) {
    const auto start = SteadyClock::now();
    while (!condition()) {
        if (elapsedMs(start) > timeout_ms) {
            return false;
        }
        pauseMs(10);
    }
    return true;
}

void sendText(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent, kSendFlags);
        if (written <= 0) {
            return;  // the client hung up
        }
        sent += static_cast<std::size_t>(written);
    }
}

std::string lowerText(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

struct SeenRequest {
    std::string method;
    std::string path;
    /// Request line and headers.
    std::string head;
    std::string body;
};

/// Value of the first `name` header in a request head, empty when absent.
std::string headerOf(const std::string& head, const std::string& name) {
    std::size_t begin = head.find("\r\n");
    while (begin != std::string::npos) {
        begin += 2;
        const std::size_t end = head.find("\r\n", begin);
        if (end == std::string::npos || end == begin) {
            return {};
        }
        const std::string line = head.substr(begin, end - begin);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos && lowerText(line.substr(0, colon)) == lowerText(name)) {
            const std::size_t value = line.find_first_not_of(' ', colon + 1);
            return value == std::string::npos ? std::string() : line.substr(value);
        }
        begin = end;
    }
    return {};
}

/// Reads one request (head plus Content-Length body). False when the client closed first or
/// nothing complete arrived within `timeout_ms`.
bool readRequest(int fd, SeenRequest& request, int timeout_ms) {
    const auto start = SteadyClock::now();
    std::string buffer;
    for (;;) {
        const std::size_t end = buffer.find("\r\n\r\n");
        if (end != std::string::npos) {
            const std::string head = buffer.substr(0, end + 2);
            const std::string length_text = headerOf(head, "Content-Length");
            const std::size_t length = length_text.empty() ? 0 : std::stoul(length_text);
            if (buffer.size() >= end + 4 + length) {
                request.head = head;
                request.body = buffer.substr(end + 4, length);
                const std::size_t space = head.find(' ');
                const std::size_t second = head.find(' ', space + 1);
                request.method = head.substr(0, space);
                request.path = head.substr(space + 1, second - space - 1);
                return true;
            }
        }
        const int left = timeout_ms - static_cast<int>(elapsedMs(start));
        pollfd entry{};
        entry.fd = fd;
        entry.events = POLLIN;
        if (left <= 0 || ::poll(&entry, 1, left) <= 0) {
            return false;
        }
        char chunk[4096];
        const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
        if (received <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
    }
}

/// A parameter of a digest Authorization header, quoted or bare.
std::string digestParam(const std::string& header, const std::string& key) {
    std::size_t at = 0;
    while ((at = header.find(key + "=", at)) != std::string::npos) {
        const bool starts = at == 0 || header[at - 1] == ' ' || header[at - 1] == ',';
        at += key.size() + 1;
        if (!starts) {
            continue;
        }
        if (at < header.size() && header[at] == '"') {
            const std::size_t close = header.find('"', at + 1);
            return header.substr(at + 1, close - at - 1);
        }
        const std::size_t end = header.find(',', at);
        return header.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }
    return {};
}

/// Checks a digest Authorization the way a camera does, from the RFC 2617 formulas.
bool digestAccepted(const SeenRequest& request, const Credentials& expected) {
    const std::string header = headerOf(request.head, "Authorization");
    if (header.compare(0, 7, "Digest ") != 0 ||
        digestParam(header, "username") != expected.username ||
        digestParam(header, "nonce") != kDigestNonce ||
        digestParam(header, "uri") != request.path) {
        return false;
    }
    const std::string ha1 = anpr::net::md5Hex(expected.username + ":" +
                                              digestParam(header, "realm") + ":" +
                                              expected.password);
    const std::string ha2 = anpr::net::md5Hex(request.method + ":" + request.path);
    const std::string qop = digestParam(header, "qop");
    const std::string response =
        qop.empty() ? anpr::net::md5Hex(ha1 + ":" + kDigestNonce + ":" + ha2)
                    : anpr::net::md5Hex(ha1 + ":" + kDigestNonce + ":" +
                                        digestParam(header, "nc") + ":" +
                                        digestParam(header, "cnonce") + ":" + qop + ":" + ha2);
    return digestParam(header, "response") == response;
}

std::string httpAnswer(const std::string& status, const std::string& headers,
                       const std::string& body) {
    return "HTTP/1.1 " + status + "\r\n" + headers +
           "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
           body;
}

/// What a Hikvision camera answers to a request without (valid) credentials.
std::string digestChallenge() {
    return httpAnswer("401 Unauthorized",
                      "WWW-Authenticate: Digest qop=\"auth\", realm=\"IP Camera(C2190)\", "
                      "nonce=\"" + kDigestNonce + "\", stale=\"FALSE\"\r\n",
                      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><userCheck><statusValue>401"
                      "</statusValue><statusString>Unauthorized</statusString></userCheck>");
}

const std::string kNotSupported =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ResponseStatus version=\"2.0\">"
    "<statusCode>4</statusCode><statusString>Invalid Operation</statusString>"
    "<subStatusCode>notSupport</subStatusCode></ResponseStatus>";

/// A scripted HTTP server on 127.0.0.1: one connection at a time and one request per connection
/// (the clients send "Connection: close"). The handler answers; a streaming handler keeps the
/// connection with holdUntilHangUp().
class FakeHttpServer {
public:
    using Handler = std::function<void(FakeHttpServer& server, int fd, const SeenRequest& request)>;

    explicit FakeHttpServer(Handler handler) : handler_(std::move(handler)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listen_fd_, 16);
        socklen_t length = sizeof(address);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this]() { run(); });
    }
    ~FakeHttpServer() {
        stop();
        ::close(listen_fd_);
    }
    FakeHttpServer(const FakeHttpServer&) = delete;
    FakeHttpServer& operator=(const FakeHttpServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] bool stopping() const { return stop_.load(); }
    /// Connections accepted so far, the current one included.
    [[nodiscard]] int connections() const { return connections_.load(); }

    void stop() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::vector<SeenRequest> requests() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    /// Requests that carried credentials (an Authorization header).
    int authorizedRequests() const {
        int count = 0;
        for (const SeenRequest& request : requests()) {
            count += headerOf(request.head, "Authorization").empty() ? 0 : 1;
        }
        return count;
    }

    int requestsTo(const std::string& path) const {
        int count = 0;
        for (const SeenRequest& request : requests()) {
            count += request.path == path ? 1 : 0;
        }
        return count;
    }

    /// Keeps a streaming connection until the client closes it or the server stops.
    void holdUntilHangUp(int fd) const {
        char buffer[512];
        while (!stopping()) {
            pollfd entry{};
            entry.fd = fd;
            entry.events = POLLIN;
            if (::poll(&entry, 1, 20) > 0 && ::recv(fd, buffer, sizeof(buffer), 0) <= 0) {
                return;
            }
        }
    }

private:
    void run() {
        while (!stop_.load()) {
            pollfd entry{};
            entry.fd = listen_fd_;
            entry.events = POLLIN;
            if (::poll(&entry, 1, 20) <= 0) {
                continue;
            }
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
#ifdef SO_NOSIGPIPE
            const int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            ++connections_;
            SeenRequest request;
            if (readRequest(fd, request, 2000)) {
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    requests_.push_back(request);
                }
                handler_(*this, fd, request);
            }
            ::close(fd);
        }
    }

    Handler handler_;
    int listen_fd_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::atomic<int> connections_{0};
    mutable std::mutex mutex_;
    std::vector<SeenRequest> requests_;
    std::thread thread_;
};

/// A port on 127.0.0.1 where nothing listens.
std::uint16_t refusedPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    ::close(fd);
    return ntohs(address.sin_port);
}

// --- ONVIF fixtures ---

std::string soapAnswer(const std::string& body) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<env:Envelope xmlns:env=\"http://www.w3.org/2003/05/soap-envelope\""
           " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
           " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
           " xmlns:ter=\"http://www.onvif.org/ver10/error\">"
           "<env:Body>" +
           body + "</env:Body></env:Envelope>";
}

/// GetSystemDateAndTimeResponse reporting `unix_seconds` as the camera's UTC clock.
std::string onvifClock(std::int64_t unix_seconds) {
    const auto time = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
    gmtime_r(&time, &utc);
    const auto number = [](int value) { return std::to_string(value); };
    return soapAnswer(
        "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
        "<tt:DateTimeType>Manual</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>"
        "<tt:TimeZone><tt:TZ>CST-5:00:00</tt:TZ></tt:TimeZone><tt:UTCDateTime><tt:Time>"
        "<tt:Hour>" + number(utc.tm_hour) + "</tt:Hour><tt:Minute>" + number(utc.tm_min) +
        "</tt:Minute><tt:Second>" + number(utc.tm_sec) + "</tt:Second></tt:Time><tt:Date>"
        "<tt:Year>" + number(utc.tm_year + 1900) + "</tt:Year><tt:Month>" +
        number(utc.tm_mon + 1) + "</tt:Month><tt:Day>" + number(utc.tm_mday) +
        "</tt:Day></tt:Date></tt:UTCDateTime></tds:SystemDateAndTime>"
        "</tds:GetSystemDateAndTimeResponse>");
}

const std::string kOnvifDeviceInformation = soapAnswer(
    "<tds:GetDeviceInformationResponse><tds:Manufacturer>HIKVISION</tds:Manufacturer>"
    "<tds:Model>DS-TCG406-E</tds:Model><tds:FirmwareVersion>V5.7.18 build 240826"
    "</tds:FirmwareVersion><tds:SerialNumber>DS-TCG406-E20240512AAWRFA1234567"
    "</tds:SerialNumber><tds:HardwareId>88</tds:HardwareId></tds:GetDeviceInformationResponse>");

const std::string kOnvifNotAuthorized = soapAnswer(
    "<env:Fault><env:Code><env:Value>env:Sender</env:Value><env:Subcode>"
    "<env:Value>ter:NotAuthorized</env:Value></env:Subcode></env:Code><env:Reason>"
    "<env:Text xml:lang=\"en\">The action requested requires authorization and the sender is "
    "not authorized</env:Text></env:Reason></env:Fault>");

std::int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Unix seconds of "YYYY-MM-DDTHH:MM:SSZ", -1 when malformed.
std::int64_t parseCreated(const std::string& text) {
    std::tm utc{};
    char zone = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &utc.tm_year, &utc.tm_mon,
                    &utc.tm_mday, &utc.tm_hour, &utc.tm_min, &utc.tm_sec, &zone) != 7 ||
        zone != 'Z' || text.size() != 20) {
        return -1;
    }
    utc.tm_year -= 1900;
    utc.tm_mon -= 1;
    return static_cast<std::int64_t>(timegm(&utc));
}

/// What a camera checks in a WS-Security UsernameToken, recomputed independently.
bool usernameTokenValid(const std::string& body, const Credentials& expected) {
    const auto text = [&body](const char* name) {
        return anpr::xml::firstText(body, name).value_or("");
    };
    const auto nonce = anpr::net::base64Decode(text("Nonce"));
    if (!nonce || nonce->size() != 16 || text("Username") != expected.username) {
        return false;
    }
    const std::string digest = anpr::net::base64Encode(
        anpr::net::sha1Raw(*nonce + text("Created") + expected.password));
    return text("Password") == digest;
}

// --- Native ANPR fixtures ---

std::string alertPart(const std::string& content_type, const std::string& body,
                      bool with_length) {
    std::string part = "--boundary\r\nContent-Type: " + content_type + "\r\n";
    if (with_length) {
        part += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    return part + "\r\n" + body + "\r\n";
}

/// A second car: another plate, Kazakhstan, another time.
std::string secondPlateAlert() {
    std::string alert = replaceAll(kAnprAlert, "ZG6140G", "777ABC02");
    alert = replaceAll(alert, "<country>19</country>", "<country>30</country>");
    return replaceAll(alert, "2023-11-02T21:10:32+01:00", "2023-11-02T21:11:05+01:00");
}

/// Sends `body` with chunked transfer coding in pieces of `piece` bytes, so chunk boundaries
/// fall inside multipart headers and bodies.
void sendChunked(int fd, const std::string& body, std::size_t piece) {
    for (std::size_t offset = 0; offset < body.size(); offset += piece) {
        const std::string chunk = body.substr(offset, piece);
        char size[32];
        std::snprintf(size, sizeof(size), "%zx\r\n", chunk.size());
        sendText(fd, size + chunk + "\r\n");
    }
}

NativeAnprListener::Options listenerOptions(std::uint16_t port, const Credentials& credentials) {
    NativeAnprListener::Options options;
    options.camera_id = "camera-02";
    options.host = kLocalhost;
    options.http_port = port;
    options.credentials = credentials;
    options.connect_timeout_ms = 2000;
    options.idle_timeout_ms = 5000;
    options.reconnect_initial_ms = 50;
    options.reconnect_max_ms = 200;
    return options;
}

/// Collects the listener's events across threads.
class EventLog {
public:
    void add(const NativePlateEvent& event) {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
    }
    std::vector<NativePlateEvent> events() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }
    std::size_t size() const { return events().size(); }

private:
    mutable std::mutex mutex_;
    std::vector<NativePlateEvent> events_;
};

// --- Raw packets for the SADP capture ---

void putShort(std::string& bytes, std::size_t at, unsigned value) {
    bytes[at] = static_cast<char>((value >> 8U) & 0xFFU);
    bytes[at + 1] = static_cast<char>(value & 0xFFU);
}

std::string udpDatagram(unsigned source_port, unsigned destination_port,
                        const std::string& payload) {
    std::string udp(8, '\0');
    putShort(udp, 0, source_port);
    putShort(udp, 2, destination_port);
    putShort(udp, 4, static_cast<unsigned>(payload.size() + 8));
    return udp + payload;
}

/// An IPv4 packet from `source` to 192.168.10.5. `fragment` is the flags/offset field (0x2000:
/// more fragments; the low 13 bits: offset in 8-byte units).
std::string ipv4Packet(Ipv4 source, unsigned identification, unsigned fragment,
                       unsigned protocol, const std::string& payload, std::size_t option_words) {
    const std::size_t header = 20 + option_words * 4;
    std::string packet(header, '\0');
    packet[0] = static_cast<char>(0x40U | static_cast<unsigned>(5 + option_words));
    putShort(packet, 2, static_cast<unsigned>(header + payload.size()));
    putShort(packet, 4, identification);
    putShort(packet, 6, fragment);
    packet[8] = 64;
    packet[9] = static_cast<char>(protocol);
    putShort(packet, 12, source.value >> 16U);
    putShort(packet, 14, source.value & 0xFFFFU);
    putShort(packet, 16, 0xC0A8U);
    putShort(packet, 18, 0x0A05U);
    return packet + payload;
}

void feed(anpr::hikvision::detail::SadpPacketCollector& collector, const std::string& packet) {
    collector.addIpv4Packet(reinterpret_cast<const unsigned char*>(packet.data()), packet.size());
}

}  // namespace

// --- SADP merge, packet capture, discovery without an interface ---

TEST("sadp merge joins repeated answers and keeps a duplicate address apart") {
    using anpr::hikvision::parseSadpResponse;
    // The plain inquiry's answer lacks the software version that the v32 answer carries.
    std::string plain_text = kProbeMatch;
    plain_text.erase(plain_text.find("<SoftwareVersion>"),
                     plain_text.find("<DSPVersion>") - plain_text.find("<SoftwareVersion>"));
    const std::string v32_text = replaceAll(kProbeMatch, "<Types>inquiry</Types>",
                                            "<Types>inquiry_v32</Types><EZVIZCode>X</EZVIZCode>");
    // A second factory-reset camera on the same default address.
    std::string twin_text = replaceAll(kProbeMatch, "c4-2f-90-a7-b5-d1", "c4-2f-90-00-00-02");
    twin_text = replaceAll(twin_text, "AAWRFA1234567", "AAWRFA7654321");
    // An activated camera on the camera subnet.
    const std::string static_text =
        replaceAll(replaceAll(kProbeMatch, "192.168.1.64", "192.168.10.21"), "c4-2f-90-a7-b5-d1",
                   "c4-2f-90-11-22-33");

    std::vector<anpr::hikvision::SadpDevice> answers;
    const std::vector<std::string> texts = {static_text, plain_text, twin_text,
                                            v32_text,    plain_text, static_text};
    for (const std::string& text : texts) {
        const auto device = parseSadpResponse(text, ip("192.168.1.64"));
        CHECK(device.has_value());
        answers.push_back(*device);
    }
    const auto merged = anpr::hikvision::mergeSadpAnswers(answers);
    CHECK_EQ(merged.size(), std::size_t{3});
    CHECK_EQ(merged[0].ipv4, std::string("192.168.1.64"));
    CHECK_EQ(merged[0].mac, std::string("c4:2f:90:00:00:02"));
    CHECK_EQ(merged[0].serial, std::string("DS-TCG406-E20240512AAWRFA7654321"));
    CHECK_EQ(merged[1].ipv4, std::string("192.168.1.64"));
    CHECK_EQ(merged[1].mac, std::string("c4:2f:90:a7:b5:d1"));
    CHECK_EQ(merged[1].software_version, std::string("V5.6.10build 230721"));
    CHECK_EQ(field(merged[1], "EZVIZCode"), std::string("X"));
    CHECK_EQ(field(merged[1], "Types"), std::string("inquiry"));
    CHECK_EQ(merged[2].ipv4, std::string("192.168.10.21"));
    CHECK(anpr::hikvision::mergeSadpAnswers({}).empty());
}

TEST("sadp packet collector extracts answers from raw IPv4 packets") {
    anpr::hikvision::detail::SadpPacketCollector collector;
    const Ipv4 camera = ip("192.168.1.64");
    const std::string answer = udpDatagram(37020, 37020, kProbeMatch);
    // IP options and Ethernet padding after the packet.
    feed(collector, ipv4Packet(camera, 1, 0, 17, answer, 2) + std::string(10, '\0'));
    // Not SADP: another source port, TCP, a truncated packet, a non-IPv4 version, garbage.
    feed(collector, ipv4Packet(camera, 2, 0, 17, udpDatagram(3702, 37020, kProbeMatch), 0));
    feed(collector, ipv4Packet(camera, 3, 0, 6, answer, 0));
    const std::string whole = ipv4Packet(camera, 4, 0, 17, answer, 0);
    feed(collector, whole.substr(0, whole.size() - 1));
    std::string version6 = whole;
    version6[0] = 0x65;
    feed(collector, version6);
    feed(collector, std::string("\x45\x00\x00", 3));
    collector.addIpv4Packet(nullptr, 0);
    CHECK_EQ(collector.datagramCount(), std::size_t{1});

    // A fragmented answer, pieces out of order and one repeated: delivered once, complete.
    const Ipv4 other = ip("192.168.1.65");
    const std::string pieces[3] = {answer.substr(0, 400), answer.substr(400, 400),
                                   answer.substr(800)};
    feed(collector, ipv4Packet(other, 77, 800 / 8, 17, pieces[2], 0));
    feed(collector, ipv4Packet(other, 77, 0x2000U, 17, pieces[0], 0));
    feed(collector, ipv4Packet(other, 77, 0x2000U, 17, pieces[0], 0));
    CHECK_EQ(collector.datagramCount(), std::size_t{1});
    CHECK_EQ(collector.pendingFragmentGroups(), std::size_t{1});
    feed(collector, ipv4Packet(other, 77, 0x2000U | (400 / 8), 17, pieces[1], 0));
    CHECK_EQ(collector.pendingFragmentGroups(), std::size_t{0});
    feed(collector, ipv4Packet(other, 77, 0x2000U | (400 / 8), 17, pieces[1], 0));

    // Later fragments of other traffic never complete and stay bounded.
    for (unsigned id = 1000; id < 1100; ++id) {
        feed(collector, ipv4Packet(other, id, 0x2000U | 25, 17, std::string(64, 'x'), 0));
    }
    CHECK(collector.pendingFragmentGroups() <=
          anpr::hikvision::detail::SadpPacketCollector::kMaxFragmentGroups);
    // A fragment reaching past 64 KiB is dropped.
    feed(collector, ipv4Packet(other, 5, 8190, 17, std::string(100, 'y'), 0));

    const auto datagrams = collector.take();
    CHECK_EQ(datagrams.size(), std::size_t{2});
    CHECK(datagrams[0].from == camera);
    CHECK_EQ(datagrams[0].from_port, std::uint16_t{37020});
    CHECK_EQ(datagrams[0].payload, kProbeMatch);
    CHECK(datagrams[1].from == other);
    CHECK_EQ(datagrams[1].payload, kProbeMatch);
    CHECK(anpr::hikvision::parseSadpResponse(datagrams[1].payload, other).has_value());
    CHECK_EQ(collector.datagramCount(), std::size_t{0});
}

TEST("sadp and WS-Discovery report a missing interface without sending anything") {
    const auto start = SteadyClock::now();
    const auto sadp = anpr::hikvision::sadpDiscover("kzanpr-none0", Ipv4{}, 300);
    CHECK(!sadp.ran);
    CHECK(sadp.error.find("no such network interface") != std::string::npos);
    CHECK(sadp.devices.empty());
    const auto onvif = anpr::hikvision::wsDiscover("kzanpr-none0", Ipv4{}, 300);
    CHECK(!onvif.ran);
    CHECK(onvif.error.find("no such network interface") != std::string::npos);
    CHECK(onvif.matches.empty());
    CHECK(elapsedMs(start) < 250);
}

// --- ONVIF ---

TEST("onvif password digest matches the ONVIF programmer's guide example") {
    using anpr::hikvision::onvifPasswordDigest;
    // ONVIF Application Programmer's Guide, UsernameToken example (password "userpassword").
    const auto nonce = anpr::net::base64Decode("LKqI6G/AikKCQrN0zqZFlg==");
    CHECK(nonce.has_value());
    CHECK_EQ(onvifPasswordDigest(*nonce, "2010-09-16T07:50:45Z", "userpassword"),
             std::string("tuOSpGlFlIXsozq4HFNeeGeFLEI="));
    // Binary nonce bytes (NUL included) and markup in the password go into the hash unchanged.
    const std::string raw("\x00\x01\xFE\xFF<&>\"", 8);
    const std::string created = "2026-10-09T07:15:30Z";
    CHECK_EQ(onvifPasswordDigest(raw, created, kOnvifLogin.password),
             anpr::net::base64Encode(anpr::net::sha1Raw(raw + created + kOnvifLogin.password)));
    CHECK_EQ(onvifPasswordDigest(raw, created, kOnvifLogin.password).size(), std::size_t{28});
    CHECK(onvifPasswordDigest(raw, created, "a") != onvifPasswordDigest(raw, created, "b"));
}

TEST("onvif probe without credentials reads the clock only") {
    FakeHttpServer server([](FakeHttpServer&, int fd, const SeenRequest& request) {
        if (request.method == "POST" && request.path == "/onvif/device_service" &&
            request.body.find("GetSystemDateAndTime") != std::string::npos) {
            sendText(fd, httpAnswer("200 OK", "Content-Type: application/soap+xml\r\n",
                                    onvifClock(unixNow())));
            return;
        }
        sendText(fd, httpAnswer("400 Bad Request", "", kOnvifNotAuthorized));
    });
    const auto info = anpr::hikvision::onvifProbeDevice(kLocalhost, server.port(),
                                                        "/onvif/device_service", nullptr, 2000);
    CHECK(info.state == OnvifState::kAvailable);
    CHECK(info.detail.find("ONVIF_USERNAME") != std::string::npos);
    CHECK(info.model.empty());
    // An empty account is no account.
    const Credentials nobody;
    const auto again = anpr::hikvision::onvifProbeDevice(kLocalhost, server.port(), "", &nobody,
                                                         2000);
    CHECK(again.state == OnvifState::kAvailable);
    server.stop();
    const auto requests = server.requests();
    CHECK_EQ(requests.size(), std::size_t{2});
    for (const SeenRequest& request : requests) {
        CHECK(headerOf(request.head, "Content-Type")
                  .compare(0, 35, "application/soap+xml; charset=utf-8") == 0);
        CHECK(request.body.find("Security") == std::string::npos);
        CHECK(headerOf(request.head, "Authorization").empty());
    }
}

TEST("onvif probe identifies the camera with a WS-Security digest on the camera's clock") {
    // The camera clock is an hour ahead (no NTP on the isolated LAN).
    const std::int64_t skew = 3600;
    std::atomic<std::int64_t> created_offset{-1000000};
    FakeHttpServer server([&](FakeHttpServer&, int fd, const SeenRequest& request) {
        if (request.body.find("GetSystemDateAndTime") != std::string::npos) {
            sendText(fd, httpAnswer("200 OK", "", onvifClock(unixNow() + skew)));
            return;
        }
        const std::int64_t created =
            parseCreated(anpr::xml::firstText(request.body, "Created").value_or(""));
        created_offset = created - unixNow();
        const bool fresh = created >= 0 && std::llabs(created - (unixNow() + skew)) <= 5;
        if (request.body.find("GetDeviceInformation") != std::string::npos && fresh &&
            usernameTokenValid(request.body, kOnvifLogin)) {
            sendText(fd, httpAnswer("200 OK", "", kOnvifDeviceInformation));
            return;
        }
        sendText(fd, httpAnswer("400 Bad Request", "", kOnvifNotAuthorized));
    });
    const auto info = anpr::hikvision::onvifProbeDevice(kLocalhost, server.port(),
                                                        "/onvif/device_service", &kOnvifLogin,
                                                        2000);
    CHECK(info.state == OnvifState::kAvailable);
    CHECK_EQ(info.manufacturer, std::string("HIKVISION"));
    CHECK_EQ(info.model, std::string("DS-TCG406-E"));
    CHECK_EQ(info.firmware, std::string("V5.7.18 build 240826"));
    CHECK_EQ(info.serial, std::string("DS-TCG406-E20240512AAWRFA1234567"));
    CHECK_EQ(info.hardware_id, std::string("88"));
    CHECK(std::llabs(created_offset.load() - skew) <= 5);
    server.stop();
    const auto requests = server.requests();
    CHECK_EQ(requests.size(), std::size_t{2});
    const std::string& token_request = requests[1].body;
    // The password never travels; the escaped username does.
    CHECK(token_request.find(kOnvifLogin.password) == std::string::npos);
    CHECK(token_request.find("onvif-user") != std::string::npos);
    CHECK(token_request.find("#PasswordDigest") != std::string::npos);
    CHECK(info.detail.find(kOnvifLogin.password) == std::string::npos);
}

TEST("onvif probe sends rejected credentials once") {
    FakeHttpServer server([](FakeHttpServer&, int fd, const SeenRequest& request) {
        if (request.body.find("GetSystemDateAndTime") != std::string::npos) {
            sendText(fd, httpAnswer("200 OK", "", onvifClock(unixNow())));
            return;
        }
        sendText(fd, httpAnswer("400 Bad Request", "", kOnvifNotAuthorized));
    });
    const Credentials wrong{"onvif-user", "not-it"};
    const auto info = anpr::hikvision::onvifProbeDevice(kLocalhost, server.port(),
                                                        "/onvif/device_service", &wrong, 2000);
    CHECK(info.state == OnvifState::kAuthRequired);
    CHECK(info.detail.find("rejected") != std::string::npos);
    CHECK(info.detail.find("not-it") == std::string::npos);
    pauseMs(50);
    server.stop();
    CHECK_EQ(server.connections(), 2);
    int token_requests = 0;
    for (const SeenRequest& request : server.requests()) {
        token_requests += request.body.find("UsernameToken") != std::string::npos ? 1 : 0;
    }
    CHECK_EQ(token_requests, 1);

    // HTTP 401 without a SOAP body means the same.
    FakeHttpServer plain([](FakeHttpServer&, int fd, const SeenRequest& request) {
        if (request.body.find("GetSystemDateAndTime") != std::string::npos) {
            sendText(fd, httpAnswer("200 OK", "", onvifClock(unixNow())));
            return;
        }
        sendText(fd, httpAnswer("401 Unauthorized", "", ""));
    });
    const auto rejected = anpr::hikvision::onvifProbeDevice(kLocalhost, plain.port(), "", &wrong,
                                                            2000);
    CHECK(rejected.state == OnvifState::kAuthRequired);
}

TEST("onvif probe reports a camera without ONVIF as unavailable") {
    using anpr::hikvision::onvifProbeDevice;
    const auto refused = onvifProbeDevice(kLocalhost, refusedPort(), "", &kOnvifLogin, 1000);
    CHECK(refused.state == OnvifState::kUnavailable);
    CHECK(refused.detail.find("refused") != std::string::npos);

    FakeHttpServer missing([](FakeHttpServer&, int fd, const SeenRequest&) {
        sendText(fd, httpAnswer("404 Not Found", "", kNotSupported));
    });
    const auto absent = onvifProbeDevice(kLocalhost, missing.port(), "", &kOnvifLogin, 1000);
    CHECK(absent.state == OnvifState::kUnavailable);
    CHECK(absent.detail.find("404") != std::string::npos);
    missing.stop();
    CHECK_EQ(missing.connections(), 1);  // no credentials follow a missing service

    FakeHttpServer web_page([](FakeHttpServer&, int fd, const SeenRequest&) {
        sendText(fd, httpAnswer("200 OK", "Content-Type: text/html\r\n",
                                "<html><body>login</body></html>"));
    });
    const auto html = onvifProbeDevice(kLocalhost, web_page.port(), "", nullptr, 1000);
    CHECK(html.state == OnvifState::kUnavailable);

    // A SOAP fault from the clock call still proves the service exists.
    FakeHttpServer faulty([](FakeHttpServer&, int fd, const SeenRequest&) {
        sendText(fd, httpAnswer("500 Internal Server Error", "",
                                soapAnswer("<env:Fault><env:Code><env:Value>env:Receiver"
                                           "</env:Value></env:Code><env:Reason><env:Text>"
                                           "Service busy</env:Text></env:Reason></env:Fault>")));
    });
    const auto busy = onvifProbeDevice(kLocalhost, faulty.port(), "", nullptr, 1000);
    CHECK(busy.state == OnvifState::kError);
    CHECK(busy.detail.find("Service busy") != std::string::npos);
}

// --- ISAPI ---

TEST("isapi probe reads deviceInfo, the channel and ANPR support with digest auth") {
    const std::string traffic =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><TrafficCap version=\"2.0\">"
        "<isSupportPlateRecognitionParam>true</isSupportPlateRecognitionParam></TrafficCap>";
    FakeHttpServer server([&traffic](FakeHttpServer&, int fd, const SeenRequest& request) {
        if (!digestAccepted(request, kCameraLogin)) {
            sendText(fd, digestChallenge());
        } else if (request.path == "/ISAPI/System/deviceInfo") {
            sendText(fd, httpAnswer("200 OK", "Server: webserver\r\n", kDeviceInfo));
        } else if (request.path == "/ISAPI/Streaming/channels/101") {
            sendText(fd, httpAnswer("200 OK", "", kStreamingChannel));
        } else if (request.path == "/ISAPI/Traffic/capabilities") {
            sendText(fd, httpAnswer("200 OK", "", traffic));
        } else {
            sendText(fd, httpAnswer("404 Not Found", "", kNotSupported));
        }
    });
    const auto probe =
        anpr::hikvision::isapiProbe(kLocalhost, server.port(), kCameraLogin, 101, 2000);
    CHECK(probe.status == IsapiStatus::kOk);
    CHECK(probe.device.ok);
    CHECK_EQ(probe.device.model, std::string("DS-TCG406-E"));
    CHECK_EQ(probe.device.mac, std::string("c4:2f:90:a7:b5:d1"));
    CHECK(probe.stream.ok);
    CHECK_EQ(probe.stream.codec, std::string("H.265"));
    CHECK_EQ(probe.stream.width, 2688);
    CHECK(probe.native_anpr == NativeAnprSupport::kSupported);
    CHECK_EQ(probe.server_header, std::string("webserver"));
    CHECK(probe.detail.find("model DS-TCG406-E") != std::string::npos);
    CHECK(probe.detail.find("channel 101: H.265 2688x1520 25 fps") != std::string::npos);
    CHECK(probe.detail.find("native ANPR supported") != std::string::npos);
    CHECK(probe.detail.find(kCameraLogin.password) == std::string::npos);
    server.stop();
    CHECK_EQ(server.authorizedRequests(), 3);
    CHECK_EQ(server.connections(), 6);
    CHECK_EQ(server.requestsTo("/ISAPI/Event/capabilities"), 0);
}

TEST("isapi probe asks the event capabilities of a camera without traffic functions") {
    const std::string events =
        "<EventCap><isSupportMotionDetection>true</isSupportMotionDetection>"
        "<isSupportVehicleDetection>false</isSupportVehicleDetection></EventCap>";
    FakeHttpServer server([&events](FakeHttpServer&, int fd, const SeenRequest& request) {
        if (!digestAccepted(request, kCameraLogin)) {
            sendText(fd, digestChallenge());
        } else if (request.path == "/ISAPI/System/deviceInfo") {
            sendText(fd, httpAnswer("200 OK", "", kDeviceInfo));
        } else if (request.path == "/ISAPI/Traffic/capabilities") {
            sendText(fd, httpAnswer("403 Forbidden", "", kNotSupported));
        } else if (request.path == "/ISAPI/Event/capabilities") {
            sendText(fd, httpAnswer("200 OK", "", events));
        } else {
            sendText(fd, httpAnswer("404 Not Found", "", kNotSupported));
        }
    });
    const auto probe =
        anpr::hikvision::isapiProbe(kLocalhost, server.port(), kCameraLogin, 102, 2000);
    CHECK(probe.status == IsapiStatus::kOk);
    CHECK(!probe.stream.ok);
    CHECK(probe.detail.find("channel 102: HTTP 404") != std::string::npos);
    CHECK(probe.native_anpr == NativeAnprSupport::kNotSupported);
    server.stop();
    CHECK_EQ(server.requestsTo("/ISAPI/Event/capabilities"), 2);  // challenge, then answer
}

TEST("isapi probe sends a rejected password exactly once and stops") {
    FakeHttpServer server([](FakeHttpServer&, int fd, const SeenRequest& request) {
        if (!digestAccepted(request, kCameraLogin)) {
            sendText(fd, digestChallenge());
            return;
        }
        sendText(fd, httpAnswer("200 OK", "", kDeviceInfo));
    });
    const auto probe =
        anpr::hikvision::isapiProbe(kLocalhost, server.port(), kWrongLogin, 101, 2000);
    CHECK(probe.status == IsapiStatus::kAuthFailed);
    CHECK(!probe.device.ok);
    CHECK(probe.detail.find("401") != std::string::npos);
    CHECK(probe.detail.find(kWrongLogin.password) == std::string::npos);
    pauseMs(100);
    server.stop();
    CHECK_EQ(server.authorizedRequests(), 1);
    CHECK_EQ(server.connections(), 2);
}

TEST("isapi probe reports a missing ISAPI as unavailable and skips without credentials") {
    using anpr::hikvision::isapiProbe;
    FakeHttpServer server([](FakeHttpServer&, int fd, const SeenRequest&) {
        sendText(fd, httpAnswer("404 Not Found", "Server: lighttpd\r\n", "<html>404</html>"));
    });
    const auto missing = isapiProbe(kLocalhost, server.port(), kCameraLogin, 101, 2000);
    CHECK(missing.status == IsapiStatus::kUnavailable);
    CHECK_EQ(missing.server_header, std::string("lighttpd"));
    CHECK(missing.detail.find("404") != std::string::npos);
    const auto skipped = isapiProbe(kLocalhost, server.port(), Credentials{}, 101, 2000);
    CHECK(skipped.status == IsapiStatus::kSkipped);
    server.stop();
    CHECK_EQ(server.connections(), 1);
    CHECK_EQ(server.authorizedRequests(), 0);

    const auto refused = isapiProbe(kLocalhost, refusedPort(), kCameraLogin, 101, 1000);
    CHECK(refused.status == IsapiStatus::kUnavailable);
    CHECK(refused.detail.find("refused") != std::string::npos);

    FakeHttpServer web_page([](FakeHttpServer&, int fd, const SeenRequest&) {
        sendText(fd, httpAnswer("200 OK", "", "<html><body>router</body></html>"));
    });
    const auto html = isapiProbe(kLocalhost, web_page.port(), kCameraLogin, 101, 2000);
    CHECK(html.status == IsapiStatus::kUnavailable);
}

// --- Native ANPR listener ---

TEST("native ANPR listener delivers each distinct plate once with the camera id") {
    const std::string body = alertPart("application/xml; charset=\"UTF-8\"", kHeartbeat, true) +
                             alertPart("image/jpeg", fakeJpeg(), true) +
                             alertPart("application/xml", kAnprAlert, true) +
                             alertPart("application/xml", kAnprAlert, false) +  // re-sent
                             alertPart("application/xml", secondPlateAlert(), true);
    FakeHttpServer server([&body](FakeHttpServer& self, int fd, const SeenRequest& request) {
        if (request.path != "/ISAPI/Event/notification/alertStream") {
            sendText(fd, httpAnswer("404 Not Found", "", kNotSupported));
            return;
        }
        if (!digestAccepted(request, kCameraLogin)) {
            sendText(fd, digestChallenge());
            return;
        }
        sendText(fd,
                 "HTTP/1.1 200 OK\r\nContent-Type: multipart/mixed; boundary=boundary\r\n"
                 "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n");
        sendChunked(fd, body, 333);
        self.holdUntilHangUp(fd);
    });
    EventLog log;
    NativeAnprListener listener(listenerOptions(server.port(), kCameraLogin),
                                [&log](const NativePlateEvent& event) { log.add(event); });
    CHECK(listener.state() == NativeAnprListener::State::kStopped);
    listener.start();
    CHECK(waitFor([&log]() { return log.size() >= 2; }, 3000));
    pauseMs(150);  // a duplicate delivery would have arrived by now
    CHECK(listener.state() == NativeAnprListener::State::kStreaming);
    const auto stop_start = SteadyClock::now();
    listener.stop();
    CHECK(elapsedMs(stop_start) < 1000);
    CHECK(listener.state() == NativeAnprListener::State::kStopped);

    const auto events = log.events();
    CHECK_EQ(events.size(), std::size_t{2});
    CHECK_EQ(listener.eventsReceived(), std::int64_t{2});
    CHECK_EQ(events[0].plate, std::string("ZG6140G"));
    CHECK_EQ(events[1].plate, std::string("777ABC02"));
    CHECK_EQ(events[1].country, std::string("30"));
    for (const NativePlateEvent& event : events) {
        CHECK_EQ(event.camera_id, std::string("camera-02"));
        CHECK(event.unix_time_ms > 1700000000000LL);
    }
    server.stop();
    CHECK_EQ(server.authorizedRequests(), 1);
    CHECK_EQ(server.connections(), 2);
}

TEST("native ANPR listener gives up after one rejected login") {
    FakeHttpServer server([](FakeHttpServer&, int fd, const SeenRequest& request) {
        if (!digestAccepted(request, kCameraLogin)) {
            sendText(fd, digestChallenge());
            return;
        }
        sendText(fd, httpAnswer("500 Internal Server Error", "", ""));
    });
    EventLog log;
    NativeAnprListener listener(listenerOptions(server.port(), kWrongLogin),
                                [&log](const NativePlateEvent& event) { log.add(event); });
    listener.start();
    CHECK(waitFor(
        [&listener]() { return listener.state() == NativeAnprListener::State::kAuthFailed; },
        3000));
    pauseMs(300);  // several backoff periods: no further attempt may follow
    listener.stop();
    // The verdict stays visible after stop().
    CHECK(listener.state() == NativeAnprListener::State::kAuthFailed);
    server.stop();
    CHECK_EQ(server.authorizedRequests(), 1);
    CHECK_EQ(server.connections(), 2);
    CHECK_EQ(log.size(), std::size_t{0});
}

TEST("native ANPR listener treats a missing event stream as unsupported") {
    FakeHttpServer server([](FakeHttpServer&, int fd, const SeenRequest&) {
        sendText(fd, httpAnswer("404 Not Found", "", kNotSupported));
    });
    NativeAnprListener listener(listenerOptions(server.port(), kCameraLogin), nullptr);
    listener.start();
    CHECK(waitFor(
        [&listener]() { return listener.state() == NativeAnprListener::State::kUnsupported; },
        3000));
    pauseMs(200);
    listener.stop();
    CHECK(listener.state() == NativeAnprListener::State::kUnsupported);
    server.stop();
    CHECK_EQ(server.connections(), 1);
}

TEST("native ANPR listener reconnects after a close, a silent stream and an oversized part") {
    const std::string first = alertPart("application/xml", kAnprAlert, true);
    const std::string second = alertPart("application/xml", secondPlateAlert(), true);
    FakeHttpServer server([&](FakeHttpServer& self, int fd, const SeenRequest&) {
        const int connection = self.connections();
        // No boundary in the Content-Type on the last connection: taken from the first line.
        sendText(fd, std::string("HTTP/1.1 200 OK\r\nContent-Type: multipart/mixed") +
                         (connection == 4 ? "" : "; boundary=boundary") +
                         "\r\nConnection: close\r\n\r\n");
        if (connection == 1) {
            sendText(fd, first);  // then the camera closes the connection
        } else if (connection == 2) {
            self.holdUntilHangUp(fd);  // silent: the idle timeout reconnects
        } else if (connection == 3) {
            // A picture larger than the bound and never finished: the stream is reset.
            sendText(fd, "--boundary\r\nContent-Type: image/jpeg\r\nContent-Length: 20000000"
                         "\r\n\r\n");
            const std::string block(1U << 20U, '\x55');
            for (int i = 0; i < 10 && !self.stopping(); ++i) {
                sendText(fd, block);
            }
            self.holdUntilHangUp(fd);
        } else {
            sendText(fd, second);
            self.holdUntilHangUp(fd);
        }
    });
    NativeAnprListener::Options options = listenerOptions(server.port(), kCameraLogin);
    options.idle_timeout_ms = 300;
    options.reconnect_max_ms = 100;
    EventLog log;
    NativeAnprListener listener(options,
                                [&log](const NativePlateEvent& event) { log.add(event); });
    listener.start();
    CHECK(waitFor([&log]() { return log.size() >= 2; }, 5000));
    listener.stop();
    const auto events = log.events();
    CHECK_EQ(events.size(), std::size_t{2});
    CHECK_EQ(events[0].plate, std::string("ZG6140G"));
    CHECK_EQ(events[1].plate, std::string("777ABC02"));
    server.stop();
    CHECK_EQ(server.connections(), 4);
    CHECK_EQ(server.authorizedRequests(), 0);  // the camera never asked for credentials
}

TEST("native ANPR listener state names") {
    using State = NativeAnprListener::State;
    CHECK_EQ(anpr::hikvision::toString(State::kStopped), std::string("stopped"));
    CHECK_EQ(anpr::hikvision::toString(State::kConnecting), std::string("connecting"));
    CHECK_EQ(anpr::hikvision::toString(State::kStreaming), std::string("streaming"));
    CHECK_EQ(anpr::hikvision::toString(State::kBackoff), std::string("backoff"));
    CHECK_EQ(anpr::hikvision::toString(State::kAuthFailed), std::string("auth_failed"));
    CHECK_EQ(anpr::hikvision::toString(State::kUnsupported), std::string("unsupported"));
}

TEST("native ANPR event JSON escapes text and names the country") {
    NativePlateEvent event;
    event.camera_id = "camera-\"02\"";
    event.plate = "777\\ABC\n02";
    event.country = "30";
    event.direction = "forward";
    event.confidence = 97.5;
    event.plate_color = "white";
    event.vehicle_type = "vehicle";
    event.lane = "1";
    event.camera_time = "2026-10-09T12:15:30+05:00";
    event.unix_time_ms = 1791530130120LL;
    event.channel = "1";
    const std::string json = anpr::hikvision::toJson(event);
    const std::string prefix = "{\"event\":\"hikvision_anpr\",\"source\":\"hikvision_isapi\",";
    CHECK(json.compare(0, prefix.size(), prefix) == 0);
    CHECK(json.back() == '}');
    CHECK(json.find("\"camera_id\":\"camera-\\\"02\\\"\"") != std::string::npos);
    CHECK(json.find("\"plate\":\"777\\\\ABC\\n02\"") != std::string::npos);
    CHECK(json.find("\"country\":\"30\",\"country_iso\":\"KZ\",\"country_name\":\"Kazakhstan\"") !=
          std::string::npos);
    CHECK(json.find("\"confidence\":97.5,") != std::string::npos);
    CHECK(json.find("\"time\":\"2026-10-09T07:15:30.120Z\"") != std::string::npos);
    const auto parsed = anpr::json::parse(json);
    CHECK(parsed.ok);
    CHECK_EQ(parsed.value.getString("event"), std::string("hikvision_anpr"));
    CHECK_EQ(parsed.value.getString("camera_id"), event.camera_id);
    CHECK_EQ(parsed.value.getString("plate"), event.plate);
    CHECK_EQ(parsed.value.getString("direction"), std::string("forward"));
    CHECK_EQ(parsed.value.getString("plate_color"), std::string("white"));
    CHECK_EQ(parsed.value.getString("vehicle_type"), std::string("vehicle"));
    CHECK_EQ(parsed.value.getString("lane"), std::string("1"));
    CHECK_EQ(parsed.value.getString("camera_time"), event.camera_time);
    CHECK_EQ(parsed.value.getString("channel"), std::string("1"));
    CHECK_NEAR(parsed.value.getNumber("confidence"), 97.5, 1e-9);

    // What the camera did not send is null, never an invented value.
    NativePlateEvent bare;
    bare.plate = "A123BC";
    bare.country = "255";
    const auto bare_parsed = anpr::json::parse(anpr::hikvision::toJson(bare));
    CHECK(bare_parsed.ok);
    CHECK_EQ(bare_parsed.value.getString("country"), std::string("255"));
    for (const char* key : {"country_iso", "country_name", "confidence", "time", "direction",
                            "lane", "camera_time", "channel"}) {
        const anpr::json::Value* value = bare_parsed.value.find(key);
        CHECK(value != nullptr);
        CHECK(value->isNull());
    }
    NativePlateEvent integral = bare;
    integral.confidence = 100.0;
    integral.country = "kz";
    const std::string integral_json = anpr::hikvision::toJson(integral);
    CHECK(integral_json.find("\"confidence\":100,") != std::string::npos);
    CHECK(integral_json.find("\"country_name\":\"Kazakhstan\"") != std::string::npos);
}

#ifdef __linux__
TEST("sadp discovery reports answers only a packet socket sees (Linux, CAP_NET_RAW)") {
    // Without CAP_NET_RAW (a non-root developer shell) there is no packet socket to test.
    const int raw = ::socket(AF_PACKET, SOCK_DGRAM, 0);
    if (raw < 0) {
        return;
    }
    ::close(raw);
    // An answer from 127.0.0.1 is "our own datagram" to the UDP exchange and dropped there, just
    // as rp_filter drops a foreign-subnet camera's answer: only the packet socket can report it.
    std::atomic<bool> done{false};
    std::thread camera([&done]() {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = htons(anpr::hikvision::kSadpPort);
        ::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local));
        sockaddr_in to = local;
        while (!done.load()) {
            ::sendto(fd, kProbeMatch.data(), kProbeMatch.size(), 0,
                     reinterpret_cast<sockaddr*>(&to), sizeof(to));
            pauseMs(20);
        }
        ::close(fd);
    });
    const auto result = anpr::hikvision::sadpDiscover("lo", kLocalhost, 400);
    done = true;
    camera.join();
    CHECK(result.ran);
    CHECK_EQ(result.devices.size(), std::size_t{1});
    CHECK_EQ(result.devices[0].mac, std::string("c4:2f:90:a7:b5:d1"));
    CHECK(result.devices[0].from == kLocalhost);
}
#endif
