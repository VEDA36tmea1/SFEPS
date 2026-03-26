#include "RTSPClient.h"
#include "Config.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <cstdint>
#include <string>
#include <vector>

namespace {

std::string getenv_or(const char *key, const char *fallback)
{
    const char *v = std::getenv(key);
    if (v && *v) return std::string(v);
    return std::string(fallback ? fallback : "");
}

std::string metadata_rtsp_url()
{
    return getenv_or("METADATA_RTSP_URL", RTSP_URL);
}

bool parse_rtsp_endpoint(const std::string &url, std::string &host_out, int &port_out)
{
    const std::string scheme = "rtsp://";
    if (url.rfind(scheme, 0) != 0) return false;

    const std::string rest = url.substr(scheme.size());
    const auto slash = rest.find('/');
    const std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    if (hostport.empty()) return false;

    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        host_out = hostport.substr(0, colon);
        try {
            port_out = std::stoi(hostport.substr(colon + 1));
        } catch (...) {
            return false;
        }
    } else {
        host_out = hostport;
        port_out = CAMERA_PORT;
    }
    return !host_out.empty() && port_out > 0 && port_out <= 65535;
}

std::string recv_resp(socket_t sock)
{
    char buffer[4096] = {0};
    const int n = recv(sock, buffer, static_cast<int>(sizeof(buffer) - 1), 0);
    if (n <= 0) return std::string();
    return std::string(buffer, buffer + n);
}

bool is_2xx(const std::string &resp)
{
    return resp.find("RTSP/1.0 200") != std::string::npos;
}

std::string parse_session_id(const std::string &resp)
{
    const std::string key = "Session: ";
    const size_t pos = resp.find(key);
    if (pos == std::string::npos) return "";
    const size_t begin = pos + key.size();
    size_t end = resp.find_first_of(";\r\n", begin);
    if (end == std::string::npos) end = resp.size();
    return resp.substr(begin, end - begin);
}

std::string make_setup_url(const std::string &base_url, const std::string &track_suffix)
{
    if (track_suffix.empty()) return base_url;
    if (base_url.back() == '/') return base_url + track_suffix;
    return base_url + "/" + track_suffix;
}

std::string first_line_of(const std::string &resp)
{
    const size_t eol = resp.find('\n');
    if (eol == std::string::npos) return resp;
    return resp.substr(0, eol);
}

} // namespace

RTSPClient::RTSPClient() : sock(
#ifdef _WIN32
    INVALID_SOCKET
#else
    -1
#endif
                           ),
                           last_heartbeat(0)
{
}

RTSPClient::~RTSPClient()
{
#ifdef _WIN32
    if (sock != INVALID_SOCKET) closesocket(sock);
#else
    if (sock != -1) close(sock);
#endif
}

bool RTSPClient::connectToCamera()
{
    std::string host = getenv_or("METADATA_CAMERA_IP", CAMERA_IP);
    int port = CAMERA_PORT;
    std::string host_from_url;
    int port_from_url = CAMERA_PORT;
    if (parse_rtsp_endpoint(metadata_rtsp_url(), host_from_url, port_from_url)) {
        if (std::getenv("METADATA_CAMERA_IP") == nullptr) host = host_from_url;
        port = port_from_url;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr) != 1) {
        std::cerr << "[RTSPClient] invalid metadata host: " << host << std::endl;
        return false;
    }

#ifdef _WIN32
    if (sock == INVALID_SOCKET) {
        std::cerr << "socket() failed" << std::endl;
        return false;
    }
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == SOCKET_ERROR) {
        std::cerr << "Connection failed (metadata RTSP)" << std::endl;
        return false;
    }
#else
    if (sock < 0) {
        std::cerr << "socket() failed" << std::endl;
        return false;
    }
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Connection failed (metadata RTSP)" << std::endl;
        return false;
    }
#endif
    std::cerr << "[RTSPClient] connected host=" << host << " port=" << port << std::endl;
    return true;
}

void RTSPClient::sendHandshake()
{
    const std::string rtsp_url = metadata_rtsp_url();
    const std::string video_track = getenv_or("METADATA_VIDEO_TRACK_ID", "v");
    const std::string meta_track = getenv_or("METADATA_META_TRACK_ID", "m");

    int cseq = 1;
    auto send_msg = [&](const std::string &s) {
        const auto max_len = static_cast<size_t>((std::numeric_limits<int>::max)());
        const int len = static_cast<int>(s.size() > max_len ? max_len : s.size());
        send(sock, s.c_str(), len, 0);
    };
    auto send_req = [&](const std::string &method, const std::string &url, const std::string &extra = std::string()) {
        std::string req = method + " " + url + " RTSP/1.0\r\n";
        req += "CSeq: " + std::to_string(cseq++) + "\r\n";
        req += "User-Agent: MyClient\r\n";
        if (!extra.empty()) req += extra;
        req += "\r\n";
        send_msg(req);
        return recv_resp(sock);
    };

    std::string resp = send_req("OPTIONS", rtsp_url);
    if (!is_2xx(resp)) std::cerr << "[RTSPClient] OPTIONS failed\n" << resp << std::endl;
    resp = send_req("DESCRIBE", rtsp_url, "Accept: application/sdp\r\n");
    if (!is_2xx(resp)) std::cerr << "[RTSPClient] DESCRIBE failed url=" << rtsp_url << "\n" << resp << std::endl;

    auto try_setup = [&](const std::vector<std::string> &candidates, int interleavedBase, bool needSession) {
        for (const auto &t : candidates) {
            std::string hdr = "Transport: RTP/AVP/TCP;unicast;interleaved=" + std::to_string(interleavedBase) + "-" +
                              std::to_string(interleavedBase + 1) + "\r\n";
            if (needSession && !session_id.empty()) hdr += "Session: " + session_id + "\r\n";
            const std::string r = send_req("SETUP", make_setup_url(rtsp_url, t), hdr);
            if (is_2xx(r)) {
                const std::string sid = parse_session_id(r);
                if (!sid.empty()) session_id = sid;
                std::cerr << "[RTSPClient] SETUP OK track=" << t << " sid=" << session_id << std::endl;
                return true;
            }
            std::string fl = first_line_of(r);
            std::string r_dbg = r;
            for (char& ch : r_dbg)
            {
                if (ch == '\r' || ch == '\n') ch = ' ';
            }
            if (fl.empty())
                fl = r_dbg.substr(0, 200);
            std::cerr << "[RTSPClient] SETUP failed track=" << t
                      << " resp_first=\"" << fl << "\""
                      << " url=" << make_setup_url(rtsp_url, t)
                      << std::endl;
        }
        return false;
    };

    // STM-ibvs 스타일: video는 무조건 trackID=v 한 번만 시도
    std::vector<std::string> videoCandidates{"trackID=" + video_track};
    const bool video_ok = try_setup(videoCandidates, 0, false);
    if (!video_ok)
        std::cerr << "[RTSPClient] video SETUP failed -> trying metadata setup anyway" << std::endl;

    // STM-ibvs 스타일: meta는 무조건 trackID=m 한 번만 시도
    std::vector<std::string> metaCandidates{"trackID=" + meta_track};
    if (!try_setup(metaCandidates, 2, true)) {
        std::cerr << "[RTSPClient] metadata SETUP failed -> detections will be empty" << std::endl;
        return;
    }

    std::string playHdr;
    if (!session_id.empty()) playHdr += "Session: " + session_id + "\r\n";
    playHdr += "Range: npt=0.000-\r\n";
    resp = send_req("PLAY", rtsp_url, playHdr);
    if (!is_2xx(resp)) {
        std::cerr << "[RTSPClient] PLAY failed\n" << resp << std::endl;
        return;
    }

    std::cerr << "[RTSPClient] metadata streaming started: " << rtsp_url << std::endl;
    last_heartbeat = time(NULL);
}

void RTSPClient::sendHeartbeat()
{
    const time_t now = time(NULL);
    if (now - last_heartbeat > 30) {
        const std::string rtsp_url = metadata_rtsp_url();
        std::string msg = "GET_PARAMETER " + rtsp_url + " RTSP/1.0\r\nCSeq: 99\r\nSession: " + session_id +
                          "\r\nUser-Agent: MyClient\r\n\r\n";
        const auto max_len = static_cast<size_t>((std::numeric_limits<int>::max)());
        const int len = static_cast<int>(msg.size() > max_len ? max_len : msg.size());
        send(sock, msg.c_str(), len, 0);
        last_heartbeat = now;
    }
}
