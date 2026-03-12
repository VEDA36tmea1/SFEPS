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

#include <mysql/mysql.h>

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

void print_db_json(bool fraud, std::uint64_t inserted_count, bool required_fields_ok) {
    std::cout << "{"
              << "\"fraud\":" << (fraud ? "true" : "false") << ","
              << "\"inserted_count\":" << inserted_count << ","
              << "\"required_fields_ok\":" << (required_fields_ok ? "true" : "false")
              << "}" << std::endl;
}

void print_skip_json(const std::string& reason) {
    std::cout << "{"
              << "\"skipped\":\"" << json_escape(reason) << "\""
              << "}" << std::endl;
}

void print_parse_json(const std::string& id, const std::string& text) {
    std::cout << "{"
              << "\"id\":\"" << json_escape(id) << "\","
              << "\"text\":\"" << json_escape(text) << "\""
              << "}" << std::endl;
}

std::string sql_escape(MYSQL* conn, const std::string& raw) {
    std::string out;
    out.resize(raw.size() * 2 + 1);
    const unsigned long escaped =
        mysql_real_escape_string(conn, out.data(), raw.c_str(), static_cast<unsigned long>(raw.size()));
    out.resize(static_cast<std::size_t>(escaped));
    return out;
}

bool fetch_single_row(MYSQL* conn,
                      const std::string& query,
                      std::vector<std::string>& out_row) {
    out_row.clear();
    if (mysql_query(conn, query.c_str()) != 0) return false;

    MYSQL_RES* res = mysql_store_result(conn);
    if (res == nullptr) return false;

    MYSQL_ROW row = mysql_fetch_row(res);
    if (row == nullptr) {
        mysql_free_result(res);
        return false;
    }

    const unsigned int field_count = mysql_num_fields(res);
    out_row.reserve(field_count);
    for (unsigned int i = 0; i < field_count; ++i) {
        out_row.emplace_back(row[i] ? row[i] : "");
    }

    mysql_free_result(res);
    return true;
}

int run_case(const std::string& age_group, const std::string& card_input, const std::string& object_id) {
    AnalyticsProcessor processor("", "", "", "");
    processor.running.store(true);

    AnalyticsProcessor::PendingObject pending;
    pending.object_id = object_id;
    pending.card_age_text = "0";
    pending.age_group = age_group;
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
    processor.onRfidRead(card_text);

    const std::size_t queue_size = processor.q.size();
    const bool fraud = queue_size > 0;
    const std::size_t alert_count = g_alert_messages.size();

    std::string out_object_id;
    std::string out_card_age_text;
    std::string out_age_group;
    if (!processor.q.empty()) {
        const auto& rec = processor.q.front();
        out_object_id = rec.object_id;
        out_card_age_text = rec.card_age_text;
        out_age_group = rec.age_group;
    }

    print_case_json(
        fraud, queue_size, alert_count, bbox_notified, out_object_id, out_card_age_text, out_age_group);
    return 0;
}

int run_db_case(const std::string& age_group, const std::string& card_input, const std::string& object_id) {
    const char* host = std::getenv("SFEPS_DB_HOST");
    const char* user = std::getenv("SFEPS_DB_USER");
    const char* pass = std::getenv("SFEPS_DB_PASS");
    const char* db = std::getenv("SFEPS_DB_NAME_ANALYTICS");

    if (user == nullptr || user[0] == '\0' || pass == nullptr || pass[0] == '\0' ||
        db == nullptr || db[0] == '\0') {
        print_skip_json("missing DB env (SFEPS_DB_USER/SFEPS_DB_PASS/SFEPS_DB_NAME_ANALYTICS)");
        return 0;
    }

    AnalyticsProcessor processor((host && host[0]) ? host : "localhost", user, pass, db);
    if (!processor.start()) {
        print_skip_json("AnalyticsProcessor.start() failed (DB unavailable)");
        return 0;
    }

    const std::string escaped_id = sql_escape(processor.conn, object_id);
    const std::string delete_query =
        "DELETE FROM analytics_logs WHERE object_id='" + escaped_id + "'";
    mysql_query(processor.conn, delete_query.c_str());

    AnalyticsProcessor::PendingObject pending;
    pending.object_id = object_id;
    pending.card_age_text = "0";
    pending.age_group = age_group;
    pending.is_fraud = false;
    pending.bbox_left = 5.0f;
    pending.bbox_top = 5.0f;
    pending.bbox_right = 55.0f;
    pending.bbox_bottom = 85.0f;
    pending.created_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(processor.mtx);
        processor.pending_queue.push_back(std::move(pending));
        processor.pending_object_ids.insert(object_id);
    }

    g_alert_messages.clear();
    const std::string card_text = (card_input == "__EMPTY__") ? "" : card_input;
    processor.onRfidRead(card_text);

    bool fraud = false;
    {
        std::lock_guard<std::mutex> lock(processor.mtx);
        fraud = !processor.q.empty();
    }

    std::uint64_t inserted_count = 0;
    bool required_fields_ok = false;

    for (int i = 0; i < 20; ++i) {
        const std::string count_query =
            "SELECT COUNT(*) FROM analytics_logs WHERE object_id='" + escaped_id + "' AND is_fraud=1";
        std::vector<std::string> count_row;
        if (fetch_single_row(processor.conn, count_query, count_row) && !count_row.empty()) {
            inserted_count = static_cast<std::uint64_t>(std::strtoull(count_row[0].c_str(), nullptr, 10));
            if (inserted_count > 0) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (inserted_count > 0) {
        const std::string row_query =
            "SELECT object_id, card_age_text, age_group, is_fraud, created_at "
            "FROM analytics_logs WHERE object_id='" +
            escaped_id + "' ORDER BY created_at DESC LIMIT 1";
        std::vector<std::string> row;
        if (fetch_single_row(processor.conn, row_query, row) && row.size() == 5) {
            const bool object_ok = !row[0].empty();
            const bool card_ok = !row[1].empty();
            const bool age_ok = !row[2].empty();
            const bool fraud_ok = (row[3] == "1");
            const bool time_ok = !row[4].empty();
            required_fields_ok = object_ok && card_ok && age_ok && fraud_ok && time_ok;
        }
    }

    processor.stop();
    print_db_json(fraud, inserted_count, required_fields_ok);
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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: server_event_driver <run-case|run-db-case|parse-rfid> [args...]" << std::endl;
        return 2;
    }

    const std::string mode = argv[1];

    if (mode == "run-case") {
        if (argc < 5) {
            std::cerr << "usage: run-case <age_group> <card_text|__EMPTY__> <object_id>" << std::endl;
            return 2;
        }
        return run_case(argv[2], argv[3], argv[4]);
    }

    if (mode == "run-db-case") {
        if (argc < 5) {
            std::cerr << "usage: run-db-case <age_group> <card_text|__EMPTY__> <object_id>" << std::endl;
            return 2;
        }
        return run_db_case(argv[2], argv[3], argv[4]);
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
