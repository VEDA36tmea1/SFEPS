#ifndef VIDEO_CATALOG_EVENTS_H
#define VIDEO_CATALOG_EVENTS_H

#include <cstdint>
#include <string>
#include <vector>

struct VideoCatalogRecordInfo {
    long long id = 0;
    std::string filename;
    std::string created_at;
};

struct VideoCatalogEvent {
    enum class Kind {
        Added,
        Deleted,
    };

    std::uint64_t seq = 0;
    Kind kind = Kind::Added;
    VideoCatalogRecordInfo record;
};

void seed_video_catalog_registry(const std::vector<VideoCatalogRecordInfo>& records);
std::uint64_t snapshot_video_catalog_registry(std::vector<VideoCatalogRecordInfo>& out_records);
std::uint64_t video_catalog_latest_seq();
std::uint64_t publish_video_catalog_record_added(const VideoCatalogRecordInfo& record);
bool publish_video_catalog_record_deleted_by_filename(const std::string& filename,
                                                      VideoCatalogRecordInfo* out_record = nullptr);
bool publish_video_catalog_record_deleted_by_id(long long id,
                                                VideoCatalogRecordInfo* out_record = nullptr);
bool find_video_catalog_record_by_id(long long id, VideoCatalogRecordInfo& out_record);
void collect_video_catalog_events_since(std::uint64_t after_seq,
                                        std::vector<VideoCatalogEvent>& out_events);

#endif
