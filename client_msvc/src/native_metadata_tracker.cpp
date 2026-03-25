#include "native_metadata_tracker.h"
#include "XMLParser.h"
#include "rbf_pwm_core.h"

#include <QString>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <set>

namespace {

struct Det {
    ParsedMetadataObject obj;
    cv::Rect2d box;
    cv::Point2d c;
};

} // namespace

QVariantList NativeMetadataTracker::process(const std::vector<ParsedMetadataObject> &raw, int W, int H,
                                            qint64 metaTsMs)
{
    if (W <= 0 || H <= 0)
        return {};

    std::vector<Det> dets;
    dets.reserve(raw.size());
    for (const auto &ro : raw) {
        cv::Rect r;
        if (!computeRectFromObj(ro, W, H, r))
            continue;
        Det d;
        d.obj = ro;
        d.box = cv::Rect2d(r.x, r.y, r.width, r.height);
        d.c = cv::Point2d(d.box.x + d.box.width * 0.5, d.box.y + d.box.height * 0.5);
        dets.push_back(std::move(d));
    }

    std::vector<int> detOwner(dets.size(), -1);
    std::vector<bool> trackMatched(m_tracks.size(), false);

    auto iou = [](const cv::Rect2d &a, const cv::Rect2d &b) {
        const double x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
        const double x2 = std::min(a.x + a.width, b.x + b.width);
        const double y2 = std::min(a.y + a.height, b.y + b.height);
        const double w = std::max(0.0, x2 - x1), h = std::max(0.0, y2 - y1);
        const double inter = w * h;
        if (inter <= 0.0)
            return 0.0;
        const double uni = a.area() + b.area() - inter;
        return (uni > 1e-9) ? (inter / uni) : 0.0;
    };

    std::vector<bool> detCrowded(dets.size(), false);
    for (size_t i = 0; i < dets.size(); ++i) {
        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (iou(dets[i].box, dets[j].box) > 0.35) {
                detCrowded[i] = true;
                detCrowded[j] = true;
            }
        }
    }

    for (size_t ti = 0; ti < m_tracks.size(); ++ti) {
        auto &tr = m_tracks[ti];
        cv::Rect2d pred = tr.box;
        pred.x += tr.vel.x;
        pred.y += tr.vel.y;
        const cv::Point2d predC(pred.x + pred.width * 0.5, pred.y + pred.height * 0.5);

        double bestScore = 0.0;
        int bestDi = -1;
        for (size_t di = 0; di < dets.size(); ++di) {
            if (detOwner[di] != -1)
                continue;
            const double ov = iou(pred, dets[di].box);
            const double dist = cv::norm(predC - dets[di].c);
            const bool crowded = detCrowded[di];
            const double distTerm = std::exp(-dist / (crowded ? 80.0 : 120.0));
            const double score = 0.85 * ov + 0.15 * distTerm;
            if (score > bestScore) {
                bestScore = score;
                bestDi = static_cast<int>(di);
            }
        }
        if (bestDi >= 0) {
            const bool crowded = detCrowded[bestDi];
            const double bestOv = iou(pred, dets[bestDi].box);
            const double bestDist = cv::norm(predC - dets[bestDi].c);
            const double thScore = crowded ? 0.26 : 0.18;
            const double thIou = crowded ? 0.20 : 0.10;
            const double thDist = crowded ? 95.0 : 180.0;
            bool accept = (bestScore >= thScore && bestOv >= thIou && bestDist <= thDist);

            if (!accept && tr.lock_left > 0 && tr.lock_det >= 0 && tr.lock_det < static_cast<int>(dets.size())
                && detOwner[tr.lock_det] == -1) {
                const double lkOv = iou(pred, dets[tr.lock_det].box);
                const double lkDist = cv::norm(predC - dets[tr.lock_det].c);
                if (lkOv >= 0.12 && lkDist <= 110.0) {
                    bestDi = tr.lock_det;
                    accept = true;
                }
            }

            if (!accept) {
                tr.lock_left = std::max(0, tr.lock_left - 1);
                continue;
            }

            const cv::Point2d prevC(tr.box.x + tr.box.width * 0.5, tr.box.y + tr.box.height * 0.5);
            tr.vel = 0.7 * tr.vel + 0.3 * (dets[bestDi].c - prevC);
            tr.box = dets[bestDi].box;
            tr.miss = 0;
            tr.lock_det = bestDi;
            tr.lock_left = detCrowded[bestDi] ? 4 : std::max(0, tr.lock_left - 1);
            detOwner[bestDi] = static_cast<int>(ti);
            trackMatched[ti] = true;
        }
    }

    for (size_t ti = 0; ti < m_tracks.size(); ++ti) {
        if (!trackMatched[ti])
            m_tracks[ti].miss++;
    }
    m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
                                  [](const NativeTrack &t) { return t.miss > 20; }),
                   m_tracks.end());

    for (size_t di = 0; di < dets.size(); ++di) {
        if (detOwner[di] != -1)
            continue;
        NativeTrack t;
        t.id = m_nextId++;
        t.box = dets[di].box;
        t.lock_det = static_cast<int>(di);
        m_tracks.push_back(t);
        detOwner[di] = static_cast<int>(m_tracks.size() - 1);
    }

    std::vector<ParsedMetadataObject> trackedNative;
    trackedNative.reserve(dets.size());
    for (size_t di = 0; di < dets.size(); ++di) {
        if (detOwner[di] < 0 || detOwner[di] >= static_cast<int>(m_tracks.size()))
            continue;
        auto obj = dets[di].obj;
        obj.id = "N" + std::to_string(m_tracks[detOwner[di]].id);
        trackedNative.push_back(std::move(obj));
    }

    std::set<std::string> active_ids;
    QVariantList out;
    for (const auto &obj : trackedNative) {
        cv::Rect raw_r;
        if (!computeRectFromObj(obj, W, H, raw_r))
            continue;
        active_ids.insert(obj.id);

        auto &sr = m_smooth[obj.id];
        if (!sr.init) {
            sr.x = raw_r.x;
            sr.y = raw_r.y;
            sr.w = raw_r.width;
            sr.h = raw_r.height;
            sr.init = true;
        } else {
            double dx = raw_r.x - sr.x;
            double dy = raw_r.y - sr.y;
            double dw = raw_r.width - sr.w;
            double dh = raw_r.height - sr.h;
            if (dx * dx + dy * dy > 80.0 * 80.0) {
                sr.x = raw_r.x;
                sr.y = raw_r.y;
            } else {
                sr.x += kBBoxSmoothAlpha * dx;
                sr.y += kBBoxSmoothAlpha * dy;
            }
            if (std::abs(dw) > 60.0 || std::abs(dh) > 60.0) {
                sr.w = raw_r.width;
                sr.h = raw_r.height;
            } else {
                sr.w += kBBoxSmoothAlpha * dw;
                sr.h += kBBoxSmoothAlpha * dh;
            }
        }

        const double l = sr.x / W;
        const double t = sr.y / H;
        const double r = (sr.x + sr.w) / W;
        const double b = (sr.y + sr.h) / H;

        QVariantMap m;
        m["id"] = QString::fromStdString(obj.id);
        m["type"] = QString::fromStdString(obj.type);
        m["x"] = l;
        m["y"] = t;
        m["w"] = std::max(0.0, r - l);
        m["h"] = std::max(0.0, b - t);
        m["metaTsMs"] = metaTsMs;
        out.push_back(m);
    }

    for (auto it = m_smooth.begin(); it != m_smooth.end();) {
        if (active_ids.find(it->first) == active_ids.end())
            it = m_smooth.erase(it);
        else
            ++it;
    }

    return out;
}
