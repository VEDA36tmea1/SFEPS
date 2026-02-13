#include "../include/analytics.h"
#include "../include/event_matcher.h"
#include "../include/alert.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <cstring>
#include <random>
#include <ctime>
#include "../../Camera/get_metadata/inc/Config.h"
#include <unordered_map>

AnalyticsProcessor::AnalyticsProcessor(const char* h, const char* u, const char* p, const char* d, int cam_w_, int cam_h_)
    : host(h), user(u), pass(p), db(d), cam_w(cam_w_), cam_h(cam_h_), conn(nullptr), running(false) {}

AnalyticsProcessor::~AnalyticsProcessor() {
    stop();
}

bool AnalyticsProcessor::start() {
    conn = mysql_init(NULL);
    if (!conn) return false;
    if (mysql_real_connect(conn, host, user, pass, db, 0, NULL, 0) == NULL) {
        std::cerr << "[Analytics] DB connect error: " << mysql_error(conn) << std::endl;
        mysql_close(conn); conn = nullptr; return false;
    }
    running = true;
    worker = std::thread(&AnalyticsProcessor::workerLoop, this);
    std::cout << "[Analytics] Started." << std::endl;
    return true;
}

void AnalyticsProcessor::stop() {
    running = false;
    cv.notify_one();
    if (worker.joinable()) worker.join();
    if (conn) { mysql_close(conn); conn = nullptr; }
}

void AnalyticsProcessor::publishRaw(const std::string& raw) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        q.push(raw);
    }
    cv.notify_one();
}

static inline std::string trim(const std::string &s) {
    size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size(); while (b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}

void AnalyticsProcessor::processLine(const std::string& rawLine) {
    // parse the same format as earlier parseAndLogXML: lines with optional [TAG] and parts separated by " | "
    std::string l = trim(rawLine);
    if (l.empty()) return;

    std::string tag;
    if (!l.empty() && l.front() == '[') {
        size_t p = l.find(']');
        if (p != std::string::npos) { tag = l.substr(1, p-1); l = trim(l.substr(p+1)); if (!l.empty() && l.front()=='|') l = trim(l.substr(1)); }
    }

    std::vector<std::string> parts;
    size_t start = 0;
    while (start < l.size()) {
        size_t sep = l.find(" | ", start);
        if (sep == std::string::npos) { parts.push_back(trim(l.substr(start))); break; }
        parts.push_back(trim(l.substr(start, sep-start)));
        start = sep + 3;
    }

    std::string id, type, time_str, event_str, photo_path;
    int x=0,y=0, estimated_age=0;
    for (const auto &part : parts) {
        if (part.empty()) continue;
        size_t colon = part.find(':');
        if (colon==std::string::npos) { if (event_str.empty()) event_str = part; continue; }
        std::string key = trim(part.substr(0, colon));
        std::string val = trim(part.substr(colon+1));
        if (key=="ID") id = val;
        else if (key=="Type") type = val;
        else if (key=="Pos") {
            size_t a = val.find('('); size_t b = val.find(')'); std::string coords = val;
            if (a!=std::string::npos && b!=std::string::npos && b>a) coords = val.substr(a+1, b-a-1);
            size_t comma = coords.find(','); if (comma!=std::string::npos) {
                std::string xs = trim(coords.substr(0,comma)); std::string ys = trim(coords.substr(comma+1));
                try { double nx = std::stod(xs); double ny = std::stod(ys); x = static_cast<int>(nx * cam_w); y = static_cast<int>(ny * cam_h); } catch(...){}
            }
        } else if (key=="Time") time_str = val;
        else if (key=="Event") event_str = val;
        else if (key=="Age") { try { estimated_age = std::stoi(val); } catch(...) {} }
    }

    if (type.empty()) type = "Unknown";
    if (event_str.empty()) event_str = "Detected";
    
    // If time_str is empty, use current time
    if (time_str.empty()) {
        time_t now = std::time(nullptr);
        struct tm* timeinfo = std::localtime(&now);
        char buf[80];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", timeinfo);
        time_str = std::string(buf);
    }

    // detect first/second and perform matching (no printing)
    static std::unordered_map<std::string, unsigned int> gate_last_pass_time;
    std::string lower_event = event_str; std::transform(lower_event.begin(), lower_event.end(), lower_event.begin(), ::tolower);
    size_t p_first = lower_event.find("first");
    size_t p_second = lower_event.find("second");
    if (p_first != std::string::npos) {
        std::string gate = event_str.substr(0, p_first);
        gate.erase(std::remove_if(gate.begin(), gate.end(), ::isspace), gate.end());
        // assign random age group
        static std::mt19937 rng((std::random_device())());
        static std::vector<std::string> ages = {"Adult","Senior","Youth"};
        std::uniform_int_distribution<int> dist(0, (int)ages.size()-1);
        std::string assigned = ages[dist(rng)];
        // Pass gate_id (extracted from gate string like "Gate3") and est_age
        EventMatcher::instance().register_first(gate, id.empty()?"":id, assigned, gate, estimated_age);
        // update gate last pass time
        gate_last_pass_time[event_str] = 0; // or use timestamp if available
    }
    if (p_second != std::string::npos) {
        std::string gate = event_str.substr(0, p_second);
        gate.erase(std::remove_if(gate.begin(), gate.end(), ::isspace), gate.end());
        std::string out_msg;
        bool mismatch = EventMatcher::instance().on_second(gate, out_msg);
        if (mismatch) {
            // out_msg already sent by EventMatcher via alert
        }
        // update last pass time
        gate_last_pass_time[event_str] = 0;
    }

    // store to analytics_logs table
    if (!conn) return;
    auto escape = [&](const std::string &s){ std::string out; out.resize(s.size()*2+1); unsigned long new_len = mysql_real_escape_string(conn, &out[0], s.c_str(), s.size()); out.resize(new_len); return out; };
    std::ostringstream oss;
    oss << "INSERT INTO analytics_logs (frame_time, object_type, created_at, estimated_age, photo_path, x, y, event) VALUES ('";
    oss << escape(time_str) << "', '" << escape(type) << "', NOW(), '" << estimated_age << "', '" << escape(photo_path) << "', ";
    oss << std::fixed << std::setprecision(2) << x << ", " << y << ", '" << escape(event_str) << "')";
    std::string query = oss.str();
    if (mysql_query(conn, query.c_str())) {
        std::cerr << "[Analytics DB Error] " << mysql_error(conn) << std::endl;
    }
}

void AnalyticsProcessor::workerLoop() {
    while (running) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&]{ return !q.empty() || !running; });
        if (!running && q.empty()) break;
        std::string raw = q.front(); q.pop();
        lock.unlock();
        // raw may contain multiple lines; split
        std::istringstream iss(raw);
        std::string line;
        while (std::getline(iss, line)) {
            processLine(line);
        }
    }
}
