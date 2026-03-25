#include "video_catalog_events.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace {

struct VideoCatalogRegistryState {
    std::mutex mutex;
    std::uint64_t next_seq = 1;
    std::unordered_map<long long, VideoCatalogRecordInfo> records_by_id;
    std::unordered_map<std::string, long long> id_by_filename;
    std::deque<VideoCatalogEvent> history;
};

VideoCatalogRegistryState& video_catalog_registry_state() {
    static VideoCatalogRegistryState state;
    return state;
}

void insert_record_locked(VideoCatalogRegistryState& state, const VideoCatalogRecordInfo& record) {
    if (record.id <= 0 || record.filename.empty()) return;

    const auto existing_it = state.records_by_id.find(record.id);
    if (existing_it != state.records_by_id.end()) {
        state.id_by_filename.erase(existing_it->second.filename);
    }

    state.records_by_id[record.id] = record;
    state.id_by_filename[record.filename] = record.id;
}

bool erase_record_by_id_locked(VideoCatalogRegistryState& state,
                               long long id,
                               VideoCatalogRecordInfo* out_record) {
    const auto it = state.records_by_id.find(id);
    if (it == state.records_by_id.end()) return false;

    if (out_record != nullptr) {
        *out_record = it->second;
    }
    state.id_by_filename.erase(it->second.filename);
    state.records_by_id.erase(it);
    return true;
}

void push_event_locked(VideoCatalogRegistryState& state,
                       VideoCatalogEvent::Kind kind,
                       const VideoCatalogRecordInfo& record) {
    VideoCatalogEvent event;
    event.seq = state.next_seq++;
    event.kind = kind;
    event.record = record;
    state.history.push_back(std::move(event));
}

}  // namespace

void seed_video_catalog_registry(const std::vector<VideoCatalogRecordInfo>& records) {
    auto& state = video_catalog_registry_state();
    std::lock_guard<std::mutex> lock(state.mutex);

    state.next_seq = 1;
    state.records_by_id.clear();
    state.id_by_filename.clear();
    state.history.clear();

    for (const auto& record : records) {
        insert_record_locked(state, record);
    }
}

std::uint64_t snapshot_video_catalog_registry(std::vector<VideoCatalogRecordInfo>& out_records) {
    auto& state = video_catalog_registry_state();
    std::lock_guard<std::mutex> lock(state.mutex);

    out_records.clear();
    out_records.reserve(state.records_by_id.size());
    for (const auto& entry : state.records_by_id) {
        out_records.push_back(entry.second);
    }

    std::sort(out_records.begin(), out_records.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.created_at != rhs.created_at) return lhs.created_at > rhs.created_at;
        return lhs.id > rhs.id;
    });
    return state.next_seq - 1;
}

std::uint64_t video_catalog_latest_seq() {
    auto& state = video_catalog_registry_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.next_seq - 1;
}

std::uint64_t publish_video_catalog_record_added(const VideoCatalogRecordInfo& record) {
    auto& state = video_catalog_registry_state();
    std::lock_guard<std::mutex> lock(state.mutex);

    insert_record_locked(state, record);
    push_event_locked(state, VideoCatalogEvent::Kind::Added, record);
    return state.next_seq - 1;
}

bool publish_video_catalog_record_deleted_by_filename(const std::string& filename,
                                                      VideoCatalogRecordInfo* out_record) {
    if (filename.empty()) return false;

    auto& state = video_catalog_registry_state();
    std::lock_guard<std::mutex> lock(state.mutex);

    const auto filename_it = state.id_by_filename.find(filename);
    if (filename_it == state.id_by_filename.end()) return false;

    VideoCatalogRecordInfo removed;
    if (!erase_record_by_id_locked(state, filename_it->second, &removed)) return false;

    push_event_locked(state, VideoCatalogEvent::Kind::Deleted, removed);
    if (out_record != nullptr) {
        *out_record = removed;
    }
    return true;
}

bool publish_video_catalog_record_deleted_by_id(long long id, VideoCatalogRecordInfo* out_record) {
    if (id <= 0) return false;

    auto& state = video_catalog_registry_state();
    std::lock_guard<std::mutex> lock(state.mutex);

    VideoCatalogRecordInfo removed;
    if (!erase_record_by_id_locked(state, id, &removed)) return false;

    push_event_locked(state, VideoCatalogEvent::Kind::Deleted, removed);
    if (out_record != nullptr) {
        *out_record = removed;
    }
    return true;
}

bool find_video_catalog_record_by_id(long long id, VideoCatalogRecordInfo& out_record) {
    if (id <= 0) return false;

    auto& state = video_catalog_registry_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.records_by_id.find(id);
    if (it == state.records_by_id.end()) return false;

    out_record = it->second;
    return true;
}

void collect_video_catalog_events_since(std::uint64_t after_seq,
                                        std::vector<VideoCatalogEvent>& out_events) {
    auto& state = video_catalog_registry_state();
    std::lock_guard<std::mutex> lock(state.mutex);

    out_events.clear();
    for (const auto& event : state.history) {
        if (event.seq <= after_seq) continue;
        out_events.push_back(event);
    }
}
