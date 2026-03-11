#ifndef EVENT_MATCHER_H
#define EVENT_MATCHER_H

#include <string>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <ctime>
#include <cstdint>

// Legacy module disabled on 2026-03-11.
// Current fraud detection path uses AnalyticsProcessor, not EventMatcher.
#if 0
class EventMatcher {
public:
    static EventMatcher& instance();

    // called when camera 'first' event detected with gate info and age
    void register_first(const std::string& gate, const std::string& event_id, const std::string& assigned_age, 
                        const std::string& gate_id = "", int est_age = 0);

    // called when RFID card read with card info
    void on_rfid_read(const std::string& uid, const std::string& card_text, const std::string& card_id = "");

    // called when camera 'second' event detected => returns true if mismatch and fills message
    bool on_second(const std::string& gate, std::string& out_message);

private:
    EventMatcher();
    struct Pending { 
        std::string event_id; 
        std::string assigned_age; 
        bool card_read=false; 
        bool checked=false;
        bool fraud=false;
        std::string card_text; 
        std::string card_id;
        std::string card_age_group;
        std::string gate_id;
        int est_age=0;
        std::uint64_t seq = 0;
        time_t ts; 
    };
    std::mutex mtx;
    std::unordered_map<std::string, std::deque<Pending>> pending; // keyed by gate
    std::uint64_t seq_counter = 0;
};
#endif

#endif
