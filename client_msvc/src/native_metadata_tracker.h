#pragma once

#include <QVariantList>
#include <map>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

struct ParsedMetadataObject;

class NativeMetadataTracker
{
public:
    QVariantList process(const std::vector<ParsedMetadataObject> &raw, int W, int H, qint64 metaTsMs);

private:
    struct NativeTrack {
        int id{0};
        cv::Rect2d box;
        cv::Point2d vel{0.0, 0.0};
        int miss{0};
        int lock_det{-1};
        int lock_left{0};
    };

    struct SmoothedRect {
        double x{0}, y{0}, w{0}, h{0};
        bool init{false};
    };

    std::vector<NativeTrack> m_tracks;
    int m_nextId{1};
    std::map<std::string, SmoothedRect> m_smooth;
    static constexpr double kBBoxSmoothAlpha = 0.35;
};
