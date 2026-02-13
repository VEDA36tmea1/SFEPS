#include "../include/event_matcher.h"
#include "../include/alert.h"
#include <utility>
#include <cctype>
#include <iostream>

EventMatcher& EventMatcher::instance() {
    static EventMatcher inst;
    return inst;
}

EventMatcher::EventMatcher() {}

namespace {
std::string normalize_age_text(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalpha(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

static constexpr std::time_t PENDING_TTL_SECONDS = 30;
}

void EventMatcher::register_first(const std::string& gate, const std::string& event_id, const std::string& assigned_age, 
                                   const std::string& gate_id, int est_age) {
    std::lock_guard<std::mutex> lock(mtx);
    Pending p;
    p.event_id = event_id;
    p.assigned_age = assigned_age;
    p.gate_id = gate_id;
    p.est_age = est_age;
    p.card_read = false;
    p.checked = false;
    p.fraud = false;
    p.ts = std::time(nullptr);
    p.seq = ++seq_counter;

    auto& q = pending[gate];
    q.push_back(std::move(p));
    if (q.size() > 32) q.pop_front();  // 방어: 게이트당 과도한 pending 방지
    std::cout << "[Matcher] Registered first for gate=" << gate << " id=" << event_id << " age=" << assigned_age 
              << " gate_id=" << gate_id << " est_age=" << est_age << std::endl;
}

void EventMatcher::on_rfid_read(const std::string& uid, const std::string& card_text, const std::string& card_id) {
    std::lock_guard<std::mutex> lock(mtx);
    time_t now = std::time(nullptr);

    std::string best_gate;
    std::uint64_t best_seq = 0;
    Pending* best_pending = nullptr;

    // 전체 게이트에서 card_read=false인 최신 first를 찾는다.
    for (auto& kv : pending) {
        auto& q = kv.second;
        while (!q.empty() && (now - q.front().ts) > PENDING_TTL_SECONDS) q.pop_front();
        if (q.empty()) continue;

        // 최신 first가 마지막에 쌓임
        for (auto rit = q.rbegin(); rit != q.rend(); ++rit) {
            if (!rit->card_read && rit->seq > best_seq) {
                best_seq = rit->seq;
                best_gate = kv.first;
                best_pending = &(*rit);
                break;
            }
        }
    }

    if (!best_pending) {
        std::cout << "[Matcher] No pending event to pair RFID uid=" << uid << std::endl;
        return;
    }

    Pending& p = *best_pending;
    p.card_read = true;
    p.card_text = normalize_age_text(card_text);  // "Adult", "(Adult)" -> "adult"
    p.card_id = card_id.empty() ? uid : card_id;
    p.card_age_group = p.card_text;
    p.checked = true;

    const std::string assigned_norm = normalize_age_text(p.assigned_age);
    const std::string card_norm = p.card_text;
    p.fraud = (!assigned_norm.empty() && card_norm != assigned_norm);

    if (p.fraud) {
        std::string out_message = "FRAUD|" + p.card_id + "|" + p.card_age_group + "|" + p.gate_id + "|" + std::to_string(p.est_age);
        send_alert_to_clients(out_message);
        std::cout << "[Matcher] FRAUD DETECTED at RFID: gate=" << best_gate
                  << " assigned=" << assigned_norm
                  << " card=" << card_norm
                  << " -> " << out_message << std::endl;
    } else {
        std::cout << "[Matcher] MATCH at RFID: gate=" << best_gate
                  << " assigned=" << assigned_norm
                  << " card=" << card_norm << std::endl;
    }
}

bool EventMatcher::on_second(const std::string& gate, std::string& out_message) {
    std::lock_guard<std::mutex> lock(mtx);
    time_t now = std::time(nullptr);

    auto it = pending.find(gate);
    if (it == pending.end()) return false;

    auto& q = it->second;
    while (!q.empty() && (now - q.front().ts) > PENDING_TTL_SECONDS) q.pop_front();
    if (q.empty()) {
        pending.erase(it);
        return false;
    }

    Pending p;
    bool found = false;
    while (!q.empty()) {
        p = std::move(q.front());
        q.pop_front();

        if (!p.card_read) {
            std::cout << "[Matcher] second arrived for gate=" << gate << " but no card read paired." << std::endl;
            continue;
        }
        found = true;
        break;
    }

    if (q.empty()) pending.erase(it);

    if (!found) return false;

    std::string assigned_norm = normalize_age_text(p.assigned_age);
    std::string card_norm = p.card_text;

    if (p.checked) {
        if (p.fraud) {
            out_message = "FRAUD|" + p.card_id + "|" + p.card_age_group + "|" + p.gate_id + "|" + std::to_string(p.est_age);
            // 중복 전송 방지: RFID 시점에 이미 전송했으므로 여기선 전송하지 않음
            std::cout << "[Matcher] second only: previous mismatch already sent, gate=" << gate
                      << " assigned=" << assigned_norm << " card=" << card_norm << std::endl;
            return true;
        }
        std::cout << "[Matcher] MATCH for gate=" << gate << " (assigned:" << assigned_norm << " card:" << card_norm << ")" << std::endl;
        return false;
    }

    // 방어: RFID 경로를 우회해 들어온 경우만 즉시 비교/전송
    p.fraud = (card_norm != assigned_norm);
    if (p.fraud) {
        out_message = "FRAUD|" + p.card_id + "|" + p.card_age_group + "|" + p.gate_id + "|" + std::to_string(p.est_age);
        send_alert_to_clients(out_message);
        std::cout << "[Matcher] FRAUD DETECTED at second: gate=" << gate
                  << " assigned=" << assigned_norm << " card=" << card_norm << " -> " << out_message << std::endl;
        return true;
    }

    std::cout << "[Matcher] MATCH for gate=" << gate << " (assigned:" << assigned_norm << " card:" << card_norm << ")" << std::endl;
    return false;
}
