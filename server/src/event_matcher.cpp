#include "../include/event_matcher.h"
#include "../include/alert.h"
#include <chrono>
#include <iostream>

EventMatcher& EventMatcher::instance() {
    static EventMatcher inst;
    return inst;
}

EventMatcher::EventMatcher() {}

void EventMatcher::register_first(const std::string& gate, const std::string& event_id, const std::string& assigned_age, 
                                   const std::string& gate_id, int est_age) {
    std::lock_guard<std::mutex> lock(mtx);
    Pending p;
    p.event_id = event_id;
    p.assigned_age = assigned_age;
    p.gate_id = gate_id;
    p.est_age = est_age;
    p.card_read = false;
    p.ts = std::time(nullptr);
    pending[gate] = p;
    std::cout << "[Matcher] Registered first for gate=" << gate << " id=" << event_id << " age=" << assigned_age 
              << " gate_id=" << gate_id << " est_age=" << est_age << std::endl;
}

void EventMatcher::on_rfid_read(const std::string& uid, const std::string& card_text, const std::string& card_id) {
    std::lock_guard<std::mutex> lock(mtx);
    // Find oldest pending without card_read
    time_t best_ts = 0;
    std::string best_gate;
    for (auto &kv : pending) {
        Pending &p = kv.second;
        if (!p.card_read) {
            if (best_ts==0 || p.ts < best_ts) {
                best_ts = p.ts;
                best_gate = kv.first;
            }
        }
    }
    if (!best_gate.empty()) {
        Pending &p = pending[best_gate];
        p.card_read = true;
        p.card_text = card_text;  // this is the age_group from card (Adult/Senior/Youth)
        p.card_id = card_id.empty() ? uid : card_id;  // use card_id if provided, else uid
        p.card_age_group = card_text;  // store age group separately for clarity
        std::cout << "[Matcher] Paired RFID uid=" << uid << " card_id=" << p.card_id << " age_group=" << card_text << " to gate=" << best_gate << std::endl;
    } else {
        std::cout << "[Matcher] No pending event to pair RFID uid=" << uid << std::endl;
    }
}

bool EventMatcher::on_second(const std::string& gate, std::string& out_message) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = pending.find(gate);
    if (it == pending.end()) return false;
    Pending p = it->second;
    // remove pending after evaluation
    pending.erase(it);

    if (!p.card_read) {
        // No card read -> nothing to compare
        std::cout << "[Matcher] second arrived for gate=" << gate << " but no card read paired." << std::endl;
        return false;
    }

    if (p.card_text != p.assigned_age) {
        // mismatch -> build message in format: FRAUD|cardId|ageGroup|gateId|estAge
        out_message = "FRAUD|" + p.card_id + "|" + p.card_age_group + "|" + p.gate_id + "|" + std::to_string(p.est_age);
        send_alert_to_clients(out_message);
        std::cout << "[Matcher] FRAUD DETECTED: " << out_message << std::endl;
        return true;
    }
    std::cout << "[Matcher] MATCH for gate=" << gate << " (assigned:" << p.assigned_age << " card:" << p.card_text << ")" << std::endl;
    return false;
}
