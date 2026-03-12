#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#define private public
#include "analytics.h"
#include "rfid_monitor.h"
#undef private

namespace {

std::vector<std::string> g_alert_messages;

std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

void print_case_json(bool fraud,
                     std::size_t queue_size,
                     std::size_t alert_count,
                     bool bbox_notified,
                     const std::string& object_id,
                     const std::string& card_age_text,
                     const std::string& age_group) {
    std::cout << "{"
              << "\"fraud\":" << (fraud ? "true" : "false") << ","
              << "\"queue_size\":" << queue_size << ","
              << "\"alert_count\":" << alert_count << ","
              << "\"bbox_notified\":" << (bbox_notified ? "true" : "false") << ","
              << "\"object_id\":\"" << json_escape(object_id) << "\","
              << "\"card_age_text\":\"" << json_escape(card_age_text) << "\","
              << "\"age_group\":\"" << json_escape(age_group) << "\""
              << "}" << std::endl;
}

void print_parse_json(const std::string& id, const std::string& text) {
    std::cout << "{"
              << "\"id\":\"" << json_escape(id) << "\","
              << "\"text\":\"" << json_escape(text) << "\""
              << "}" << std::endl;
}

bool alert_contains_fraud_yes(const std::string& alert_line) {
    std::size_t first = alert_line.find('|');
    if (first == std::string::npos) return false;
    std::size_t second = alert_line.find('|', first + 1);
    if (second == std::string::npos) return false;
    std::size_t third = alert_line.find('|', second + 1);
    if (third == std::string::npos) return false;
    std::size_t fourth = alert_line.find('|', third + 1);
    if (fourth == std::string::npos) return false;
    std::size_t fifth = alert_line.find('|', fourth + 1);
    const std::size_t flag_start = fourth + 1;
    const std::size_t flag_len =
        (fifth == std::string::npos) ? (alert_line.size() - flag_start) : (fifth - flag_start);
    if (flag_len == 0) return false;
    return alert_line.substr(flag_start, flag_len) == "Y";
}

std::string build_outline_event_xml(const std::string& object_id) {
    return std::string("<tt:MetadataStream UtcTime=\"2026-03-12T00:00:00Z\">") +
           "<wsnt:NotificationMessage>"
           "<tt:Message>"
           "<tt:Data>"
           "<tt:SimpleItem Name=\"RuleName\" Value=\"outline\"/>"
           "<tt:SimpleItem Name=\"State\" Value=\"true\"/>"
           "<tt:SimpleItem Name=\"ObjectId\" Value=\"" +
           object_id +
           "\"/>"
           "</tt:Data>"
           "</tt:Message>"
           "</wsnt:NotificationMessage>"
           "</tt:MetadataStream>";
}

int run_case(const std::string& age_value, const std::string& card_input, const std::string& object_id) {
    AnalyticsProcessor processor("", "", "", "");
    processor.running.store(true);

    AnalyticsProcessor::PendingObject pending;
    pending.object_id = object_id;
    pending.card_age_text = "0";
    pending.age = age_value;
    pending.enter_tag_time = "2026-03-12T00:00:00Z";
    pending.is_fraud = false;
    pending.bbox_left = 10.0f;
    pending.bbox_top = 10.0f;
    pending.bbox_right = 110.0f;
    pending.bbox_bottom = 160.0f;
    pending.created_at = std::chrono::steady_clock::now();

    processor.pending_queue.push_back(std::move(pending));
    processor.pending_object_ids.insert(object_id);

    bool bbox_notified = false;
    processor.setFraudBBoxCallback([&bbox_notified](const AnalyticsProcessor::FraudBBoxPayload&) {
        bbox_notified = true;
    });

    g_alert_messages.clear();
    const std::string card_text = (card_input == "__EMPTY__") ? "" : card_input;
    if (!card_text.empty()) {
        processor.onRfidRead(card_text);
    }

    const std::string xml = build_outline_event_xml(object_id);
    processor.publishRaw(xml);

    const std::size_t queue_size = processor.q.size();
    bool fraud = false;
    if (!processor.q.empty()) {
        fraud = processor.q.front().is_fraud;
    }
    if (!fraud && !g_alert_messages.empty()) {
        fraud = alert_contains_fraud_yes(g_alert_messages.back());
    }
    const std::size_t alert_count = g_alert_messages.size();

    std::string out_object_id;
    std::string out_card_age_text;
    std::string out_age_group;
    if (!processor.q.empty()) {
        const auto& rec = processor.q.front();
        out_object_id = rec.object_id;
        out_card_age_text = rec.card_age_text;
        out_age_group = rec.age;
    }

    print_case_json(
        fraud, queue_size, alert_count, bbox_notified, out_object_id, out_card_age_text, out_age_group);
    return 0;
}

int run_parse_rfid(const std::string& json_line) {
    std::atomic<bool> running(false);
    AnalyticsProcessor processor("", "", "", "");
    RfidMonitor monitor(running, processor);

    const std::string id = monitor.extract_json_value(json_line, "id");
    const std::string text = monitor.extract_json_value(json_line, "text");
    print_parse_json(id, text);
    return 0;
}

}  // namespace

void send_alert_to_clients(const std::string& msg) {
    g_alert_messages.push_back(msg);
}

void send_test_alert_to_clients(const std::string& msg) {
    g_alert_messages.push_back(msg);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: server_event_driver <run-case|parse-rfid> [args...]" << std::endl;
        return 2;
    }

    const std::string mode = argv[1];

    if (mode == "run-case") {
        if (argc < 5) {
            std::cerr << "usage: run-case <age> <card_text|__EMPTY__> <object_id>" << std::endl;
            return 2;
        }
        return run_case(argv[2], argv[3], argv[4]);
    }

    if (mode == "parse-rfid") {
        if (argc < 3) {
            std::cerr << "usage: parse-rfid <json-line>" << std::endl;
            return 2;
        }
        return run_parse_rfid(argv[2]);
    }

    std::cerr << "unknown mode: " << mode << std::endl;
    return 2;
}
