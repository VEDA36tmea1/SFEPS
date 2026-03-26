// IdStabilizer.cpp
// ─────────────────────────────────────────────────────────────────────
// 카메라 ObjectId 변경 문제 후처리 보정 구현
// ─────────────────────────────────────────────────────────────────────

#include "re_id.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <vector>
#include <cassert>
#include <limits>

// =====================================================================
// 외형 특징 코사인 유사도
// =====================================================================
float AppearanceFeature::cosine_similarity(const AppearanceFeature& other) const {
    if (!valid || !other.valid || dim != other.dim || dim == 0)
        return 0.0f;

    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < dim; ++i) {
        dot    += data[i] * other.data[i];
        norm_a += data[i] * data[i];
        norm_b += other.data[i] * other.data[i];
    }
    if (norm_a < 1e-9f || norm_b < 1e-9f) return 0.0f;
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

// =====================================================================
// IoU 계산
// =====================================================================
static float compute_iou(float l1, float t1, float r1, float b1,
                         float l2, float t2, float r2, float b2) {
    float xA = std::max(l1, l2), yA = std::max(t1, t2);
    float xB = std::min(r1, r2), yB = std::min(b1, b2);
    float inter = std::max(0.0f, xB - xA) * std::max(0.0f, yB - yA);
    if (inter <= 0.0f) return 0.0f;
    float a1 = (r1 - l1) * (b1 - t1);
    float a2 = (r2 - l2) * (b2 - t2);
    float uni = a1 + a2 - inter;
    return (uni > 1e-9f) ? (inter / uni) : 0.0f;
}

// =====================================================================
// 중심 거리 유사도 (지수 감쇠 — 움직임에 관대)
// =====================================================================
static float distance_similarity(float cx1, float cy1, float cx2, float cy2,
                                 float sigma) {
    float dx = cx1 - cx2, dy = cy1 - cy2;
    float dist2 = dx * dx + dy * dy;
    // 가우시안: 거리가 sigma일 때 ~0.6, 2*sigma일 때 ~0.14
    return std::exp(-dist2 / (2.0f * sigma * sigma));
}

// =====================================================================
// 헝가리안 알고리즘
// =====================================================================
static std::vector<int> hungarian_assign(const std::vector<std::vector<float>>& cost,
                                         int n_rows, int n_cols) {
    const int n = std::max(n_rows, n_cols);
    const float INF = 1e9f;

    std::vector<std::vector<float>> c(n, std::vector<float>(n, 0.0f));
    for (int i = 0; i < n_rows; ++i)
        for (int j = 0; j < n_cols; ++j)
            c[i][j] = cost[i][j];

    std::vector<float> u(n + 1, 0), v(n + 1, 0);
    std::vector<int> p(n + 1, 0), way(n + 1, 0);

    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<float> minv(n + 1, INF);
        std::vector<bool> used(n + 1, false);
        do {
            used[j0] = true;
            int i0 = p[j0], j1 = -1;
            float delta = INF;
            for (int j = 1; j <= n; ++j) {
                if (!used[j]) {
                    float cur = c[i0 - 1][j - 1] - u[i0] - v[j];
                    if (cur < minv[j]) {
                        minv[j] = cur;
                        way[j] = j0;
                    }
                    if (minv[j] < delta) {
                        delta = minv[j];
                        j1 = j;
                    }
                }
            }
            for (int j = 0; j <= n; ++j) {
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else         { minv[j] -= delta; }
            }
            j0 = j1;
        } while (p[j0] != 0);

        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }

    std::vector<int> result(n_rows, -1);
    for (int j = 1; j <= n; ++j) {
        if (p[j] != 0 && p[j] <= n_rows && j <= n_cols)
            result[p[j] - 1] = j - 1;
    }
    return result;
}

// =====================================================================
// 내부 트랙 구조체
// =====================================================================
struct IdStabilizer::Track {
    std::string stable_id;
    std::string last_camera_id;

    float cx = 0, cy = 0;
    float vx = 0, vy = 0;
    float width = 0, height = 0;
    bool  kalman_init = false;

    float left = 0, top = 0, right = 0, bottom = 0;

    double sm_x = 0, sm_y = 0, sm_w = 0, sm_h = 0;
    bool   sm_init = false;

    AppearanceFeature appearance;

    int64_t last_seen_ms = 0;
    int64_t created_ms   = 0;
    int     missed_frames = 0;

    enum State { ACTIVE, LOST, GALLERY };
    State state = ACTIVE;
    float confidence = 1.0f;

    void predict(float dt_sec, float& pred_cx, float& pred_cy) const {
        pred_cx = cx + vx * dt_sec;
        pred_cy = cy + vy * dt_sec;
    }

    void kalman_update(float meas_cx, float meas_cy, float meas_w, float meas_h,
                       float dt_sec, float alpha_pos, float beta_vel) {
        if (!kalman_init || dt_sec <= 0) {
            cx = meas_cx; cy = meas_cy;
            width = meas_w; height = meas_h;
            vx = vy = 0;
            kalman_init = true;
            return;
        }

        float px = cx + vx * dt_sec;
        float py = cy + vy * dt_sec;
        float rx = meas_cx - px;
        float ry = meas_cy - py;

        float dist2 = rx * rx + ry * ry;
        // outlier 게이팅: bbox 대각선의 4배까지 허용 (움직이는 사람 대응)
        float diag = std::sqrt(width * width + height * height);
        float max_jump = std::max(diag * 4.0f, 0.2f);  // 정규화 좌표 최소 0.2
        if (dist2 > max_jump * max_jump) {
            // 점프가 너무 크면 위치만 리셋, 속도는 방향 힌트로 남김
            cx = meas_cx; cy = meas_cy;
            width = meas_w; height = meas_h;
            // 점프 방향으로 속도 힌트 (다음 프레임 예측에 도움)
            vx = rx / std::max(dt_sec, 1e-4f) * 0.3f;
            vy = ry / std::max(dt_sec, 1e-4f) * 0.3f;
            return;
        }

        // alpha-beta 필터: alpha_pos로 위치 보정, beta_vel로 속도 학습
        cx = px + alpha_pos * rx;
        cy = py + alpha_pos * ry;
        vx += (beta_vel * rx) / std::max(dt_sec, 1e-4f);
        vy += (beta_vel * ry) / std::max(dt_sec, 1e-4f);

        // 속도 상한 (좌표계에 맞게 — 정규화면 ~2.0/s, 픽셀이면 ~3000px/s)
        float max_v = std::max(diag * 30.0f, 3.0f);
        vx = std::max(-max_v, std::min(max_v, vx));
        vy = std::max(-max_v, std::min(max_v, vy));

        width  += 0.3f * (meas_w - width);
        height += 0.3f * (meas_h - height);
    }

    void smooth_bbox(float raw_l, float raw_t, float raw_r, float raw_b,
                     double ema_alpha, double jump_threshold) {
        double raw_x = raw_l, raw_y = raw_t;
        double raw_w = raw_r - raw_l, raw_h = raw_b - raw_t;

        if (!sm_init) {
            sm_x = raw_x; sm_y = raw_y;
            sm_w = raw_w; sm_h = raw_h;
            sm_init = true;
            return;
        }

        double dx = raw_x - sm_x, dy = raw_y - sm_y;
        double dist = std::sqrt(dx * dx + dy * dy);

        // 속도가 있으면 alpha를 높여서 반응 빠르게 (움직이는 사람 대응)
        double speed = std::sqrt((double)vx * vx + (double)vy * vy);
        double a = ema_alpha;
        if (speed > 0.01) {
            // 속도에 비례해서 alpha를 올림 (최대 0.85)
            a = std::min(0.85, ema_alpha + speed * 0.5);
        }
        // 점프가 너무 크면 즉시 따라감
        if (dist > jump_threshold) {
            a = 0.9;
        }

        sm_x += a * (raw_x - sm_x);
        sm_y += a * (raw_y - sm_y);
        sm_w += a * (raw_w - sm_w);
        sm_h += a * (raw_h - sm_h);
    }

    void get_smoothed_bbox(float& out_l, float& out_t, float& out_r, float& out_b) const {
        out_l = (float)sm_x;
        out_t = (float)sm_y;
        out_r = (float)(sm_x + sm_w);
        out_b = (float)(sm_y + sm_h);
    }
};

// =====================================================================
// Impl
// =====================================================================
struct IdStabilizer::Impl {
    IdStabilizerConfig cfg;
    std::vector<Track> tracks;
    std::vector<Track> gallery;
    int next_id = 1;
    int64_t last_timestamp_ms = 0;

    std::string make_id() {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "S_%03d", next_id++);
        return std::string(buf);
    }

    float compute_match_score(const Track& track, const DetectedInput& det,
                              float dt_sec) const {
        float pred_cx, pred_cy;
        if (track.kalman_init)
            track.predict(dt_sec, pred_cx, pred_cy);
        else {
            pred_cx = track.cx;
            pred_cy = track.cy;
        }

        // IoU: 예측 bbox를 20% 확장해서 계산 (움직임 마진)
        float pred_half_w = track.width / 2.0f * 1.2f;
        float pred_half_h = track.height / 2.0f * 1.2f;
        float iou = compute_iou(
            pred_cx - pred_half_w, pred_cy - pred_half_h,
            pred_cx + pred_half_w, pred_cy + pred_half_h,
            det.left, det.top, det.right, det.bottom);

        // 거리 유사도: sigma = bbox 대각선 (정규화/픽셀 모두 대응)
        // 정규화 좌표면 sigma ~ 0.15, 픽셀이면 sigma ~ 200px
        float diag = std::sqrt(track.width * track.width + track.height * track.height);
        float sigma = std::max(diag * 1.5f, 0.05f);  // 최소 0.05 (정규화 기준)
        float dist_sim = distance_similarity(pred_cx, pred_cy, det.cx, det.cy, sigma);

        // 외형 유사도
        float app_sim = 0.0f;
        if (cfg.appearance_weight > 0 && track.appearance.valid && det.appearance.valid)
            app_sim = std::max(0.0f, track.appearance.cosine_similarity(det.appearance));

        // 가중합 — 외형이 없으면 IoU와 거리만으로
        float w_iou  = cfg.iou_weight;
        float w_dist = cfg.distance_weight;
        float w_app  = (track.appearance.valid && det.appearance.valid) ? cfg.appearance_weight : 0.0f;
        float w_sum = w_iou + w_dist + w_app;
        if (w_sum < 1e-6f) return 0.0f;

        float score = (w_iou * iou + w_dist * dist_sim + w_app * app_sim) / w_sum;

        // IoU가 0이어도 거리가 가까우면 매칭 (움직임 핵심)
        // 예: IoU=0, dist_sim=0.8 → score = 0.3*0 + 0.7*0.8 / 1.0 = 0.56
        // 이동이 커서 bbox가 안 겹쳐도 가까우면 살아남음

        // 소실 트랙은 시간 기반 감쇠
        if (track.state == Track::LOST || track.state == Track::GALLERY) {
            float age_sec = (float)track.missed_frames * 0.033f;
            score *= std::exp(-0.05f * age_sec);  // 감쇠를 느리게 (0.1 → 0.05)
        }
        return score;
    }

    std::vector<StableObject> do_update(const std::vector<DetectedInput>& dets,
                                        int64_t ts_ms) {
        float dt_sec = (last_timestamp_ms > 0)
                       ? (float)(ts_ms - last_timestamp_ms) / 1000.0f
                       : 0.033f;
        dt_sec = std::max(0.001f, std::min(1.0f, dt_sec));

        const int n_tracks = (int)tracks.size();
        const int n_dets   = (int)dets.size();

        std::vector<int> det_assignment(n_dets, -1);
        std::vector<int> track_assignment(n_tracks, -1);

        if (n_tracks > 0 && n_dets > 0) {
            std::vector<std::vector<float>> cost(n_tracks, std::vector<float>(n_dets, 1.0f));
            for (int i = 0; i < n_tracks; ++i)
                for (int j = 0; j < n_dets; ++j)
                    cost[i][j] = 1.0f - compute_match_score(tracks[i], dets[j], dt_sec);

            auto assignment = hungarian_assign(cost, n_tracks, n_dets);
            for (int i = 0; i < n_tracks; ++i) {
                int j = assignment[i];
                if (j >= 0 && j < n_dets && (1.0f - cost[i][j]) >= cfg.min_match_score) {
                    track_assignment[i] = j;
                    det_assignment[j] = i;
                }
            }
        }

        // 미매칭 탐지 → 갤러리 복구
        std::vector<int> det_gallery_match(n_dets, -1);
        for (int j = 0; j < n_dets; ++j) {
            if (det_assignment[j] >= 0) continue;
            float best_score = cfg.min_match_score;
            int best_gi = -1;
            for (int gi = 0; gi < (int)gallery.size(); ++gi) {
                float score = compute_match_score(gallery[gi], dets[j], dt_sec);
                float threshold = (gallery[gi].appearance.valid && dets[j].appearance.valid)
                                  ? cfg.min_match_score : cfg.min_match_score * 1.5f;
                if (score > best_score && score >= threshold) {
                    best_score = score;
                    best_gi = gi;
                }
            }
            if (best_gi >= 0) det_gallery_match[j] = best_gi;
        }

        // 결과 조합
        std::vector<StableObject> results;
        results.reserve(n_dets);

        // 매칭된 트랙 업데이트
        for (int i = 0; i < n_tracks; ++i) {
            int j = track_assignment[i];
            if (j < 0) continue;
            auto& trk = tracks[i];
            const auto& det = dets[j];

            trk.last_camera_id = det.camera_id;
            trk.last_seen_ms = ts_ms;
            trk.missed_frames = 0;
            trk.state = Track::ACTIVE;
            trk.confidence = det.confidence;
            trk.left = det.left; trk.top = det.top;
            trk.right = det.right; trk.bottom = det.bottom;

            trk.kalman_update(det.cx, det.cy,
                              det.right - det.left, det.bottom - det.top,
                              dt_sec, cfg.kalman_alpha_pos, cfg.kalman_beta_vel);
            trk.smooth_bbox(det.left, det.top, det.right, det.bottom,
                            cfg.bbox_ema_alpha, cfg.bbox_jump_threshold);

            if (det.appearance.valid) {
                if (!trk.appearance.valid) trk.appearance = det.appearance;
                else {
                    float a = cfg.appearance_ema;
                    for (int k = 0; k < trk.appearance.dim; ++k)
                        trk.appearance.data[k] = a * trk.appearance.data[k] + (1.0f - a) * det.appearance.data[k];
                }
            }

            StableObject obj;
            obj.stable_id = trk.stable_id;
            obj.camera_id = det.camera_id;
            trk.get_smoothed_bbox(obj.left, obj.top, obj.right, obj.bottom);
            obj.cx = (obj.left + obj.right) / 2.0f;
            obj.cy = (obj.top + obj.bottom) / 2.0f;
            obj.confidence = det.confidence;
            obj.is_recovered = false;
            results.push_back(obj);
        }

        // 갤러리 복구
        std::vector<int> gallery_used;
        for (int j = 0; j < n_dets; ++j) {
            int gi = det_gallery_match[j];
            if (gi < 0 || det_assignment[j] >= 0) continue;
            auto& trk = gallery[gi];
            const auto& det = dets[j];
            std::string old_cam = trk.last_camera_id;

            trk.last_camera_id = det.camera_id;
            trk.last_seen_ms = ts_ms;
            trk.missed_frames = 0;
            trk.state = Track::ACTIVE;
            trk.confidence = det.confidence;
            trk.left = det.left; trk.top = det.top;
            trk.right = det.right; trk.bottom = det.bottom;
            trk.kalman_init = false;
            trk.sm_init = false;
            trk.kalman_update(det.cx, det.cy, det.right - det.left, det.bottom - det.top,
                              dt_sec, cfg.kalman_alpha_pos, cfg.kalman_beta_vel);
            trk.smooth_bbox(det.left, det.top, det.right, det.bottom,
                            cfg.bbox_ema_alpha, cfg.bbox_jump_threshold);
            if (det.appearance.valid) trk.appearance = det.appearance;

            tracks.push_back(trk);
            gallery_used.push_back(gi);
            det_assignment[j] = (int)tracks.size() - 1;

            StableObject obj;
            obj.stable_id = trk.stable_id;
            obj.camera_id = det.camera_id;
            trk.get_smoothed_bbox(obj.left, obj.top, obj.right, obj.bottom);
            obj.cx = (obj.left + obj.right) / 2.0f;
            obj.cy = (obj.top + obj.bottom) / 2.0f;
            obj.confidence = det.confidence;
            obj.is_recovered = true;
            obj.recovered_from = old_cam;
            results.push_back(obj);

            std::cout << "🔗 [ID 복구] " << det.camera_id
                      << " → 안정 ID: " << trk.stable_id
                      << " (갤러리에서 복구)" << std::endl;
        }

        std::sort(gallery_used.rbegin(), gallery_used.rend());
        for (int gi : gallery_used) gallery.erase(gallery.begin() + gi);

        // 새 트랙
        for (int j = 0; j < n_dets; ++j) {
            if (det_assignment[j] >= 0) continue;
            const auto& det = dets[j];
            Track trk;
            trk.stable_id = make_id();
            trk.last_camera_id = det.camera_id;
            trk.last_seen_ms = ts_ms;
            trk.created_ms = ts_ms;
            trk.state = Track::ACTIVE;
            trk.confidence = det.confidence;
            trk.left = det.left; trk.top = det.top;
            trk.right = det.right; trk.bottom = det.bottom;
            trk.kalman_update(det.cx, det.cy, det.right - det.left, det.bottom - det.top,
                              dt_sec, cfg.kalman_alpha_pos, cfg.kalman_beta_vel);
            trk.smooth_bbox(det.left, det.top, det.right, det.bottom,
                            cfg.bbox_ema_alpha, cfg.bbox_jump_threshold);
            if (det.appearance.valid) trk.appearance = det.appearance;
            tracks.push_back(trk);

            StableObject obj;
            obj.stable_id = trk.stable_id;
            obj.camera_id = det.camera_id;
            trk.get_smoothed_bbox(obj.left, obj.top, obj.right, obj.bottom);
            obj.cx = (obj.left + obj.right) / 2.0f;
            obj.cy = (obj.top + obj.bottom) / 2.0f;
            obj.confidence = det.confidence;
            obj.is_recovered = false;
            results.push_back(obj);

            std::cout << "✨ [NEW] 안정 ID: " << trk.stable_id
                      << " | 카메라 ID: " << det.camera_id << std::endl;
        }

        // 미매칭 트랙 → LOST
        for (int i = 0; i < n_tracks; ++i) {
            if (track_assignment[i] >= 0) continue;
            auto& trk = tracks[i];
            trk.missed_frames++;
            if ((ts_ms - trk.last_seen_ms) > cfg.active_timeout_ms && trk.state == Track::ACTIVE)
                trk.state = Track::LOST;
        }

        // LOST → GALLERY 이동 또는 삭제
        auto it = tracks.begin();
        while (it != tracks.end()) {
            if (it->state == Track::LOST) {
                int64_t age = ts_ms - it->last_seen_ms;
                if (age > cfg.gallery_timeout_ms) { it = tracks.erase(it); continue; }
                if (it->appearance.valid || age < cfg.active_timeout_ms * 3) {
                    if ((int)gallery.size() < cfg.max_gallery_size) {
                        it->state = Track::GALLERY;
                        gallery.push_back(*it);
                    }
                }
                it = tracks.erase(it);
            } else { ++it; }
        }

        gallery.erase(
            std::remove_if(gallery.begin(), gallery.end(),
                [&](const Track& t) { return (ts_ms - t.last_seen_ms) > cfg.gallery_timeout_ms; }),
            gallery.end());
        while ((int)gallery.size() > cfg.max_gallery_size)
            gallery.erase(gallery.begin());

        last_timestamp_ms = ts_ms;
        return results;
    }
};

// =====================================================================
// 공개 API
// =====================================================================
IdStabilizer::IdStabilizer(const IdStabilizerConfig& cfg) {
    impl_ = new Impl();
    impl_->cfg = cfg;
}

IdStabilizer::~IdStabilizer() {
    delete impl_;
}

std::vector<StableObject> IdStabilizer::update(
    const std::vector<DetectedInput>& detections, int64_t timestamp_ms) {
    return impl_->do_update(detections, timestamp_ms);
}

void IdStabilizer::remove_track(const std::string& stable_id) {
    auto& t = impl_->tracks;
    t.erase(std::remove_if(t.begin(), t.end(),
        [&](const Track& x) { return x.stable_id == stable_id; }), t.end());
    auto& g = impl_->gallery;
    g.erase(std::remove_if(g.begin(), g.end(),
        [&](const Track& x) { return x.stable_id == stable_id; }), g.end());
}

void IdStabilizer::reset() {
    impl_->tracks.clear();
    impl_->gallery.clear();
    impl_->next_id = 1;
    impl_->last_timestamp_ms = 0;
}

int IdStabilizer::active_track_count() const {
    int c = 0;
    for (const auto& t : impl_->tracks) if (t.state == Track::ACTIVE) ++c;
    return c;
}

int IdStabilizer::gallery_track_count() const {
    return (int)impl_->gallery.size();
}
