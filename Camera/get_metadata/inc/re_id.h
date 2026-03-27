#pragma once
// IdStabilizer.h
// ─────────────────────────────────────────────────────────────────────
// 카메라 AI가 할당하는 ObjectId가 가림/이탈 후 바뀌는 문제를 후처리로 보정.
// 카메라 탐지 로직은 건드리지 않고, 파싱 결과에 대해서만 동작한다.
//
// 사용법:
//   IdStabilizer stabilizer;
//   // 매 프레임:
//   std::vector<StableObject> result = stabilizer.update(parsed_objects, timestamp_ms);
//   // result[i].stable_id 가 안정적 ID
// ─────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <cstdint>
#include <array>

struct IdStabilizerConfig {
    // --- 매칭 가중치 ---
    // 움직이면 IoU가 급락하므로 거리를 주력으로 사용
    float  iou_weight          = 0.25f;  // IoU (겹침)
    float  distance_weight     = 0.55f;  // 중심 거리 (가우시안)
    float  appearance_weight   = 0.2f;   // 외형 (없으면 자동 비활성)
    float  min_match_score     = 0.15f;  // 매칭 임계값 (낮춰서 움직임 허용)

    // --- 칼만 예측 ---
    float  kalman_alpha_pos    = 0.7f;   // 위치 보정 게인 (높을수록 측정 신뢰)
    float  kalman_beta_vel     = 0.3f;   // 속도 학습 게인 (높을수록 빠르게 반응)

    int64_t active_timeout_ms  = 3000;
    int64_t gallery_timeout_ms = 30000;
    int     max_gallery_size   = 50;

    double  bbox_ema_alpha     = 0.5;    // 스무딩 (높을수록 반응 빠름)
    double  bbox_jump_threshold= 200.0;  // 점프 임계값 (넉넉하게)

    int     appearance_dim     = 128;
    float   appearance_ema     = 0.8f;
};

static constexpr int MAX_APPEARANCE_DIM = 256;

struct AppearanceFeature {
    std::array<float, MAX_APPEARANCE_DIM> data{};
    int   dim   = 0;
    bool  valid = false;
    float cosine_similarity(const AppearanceFeature& other) const;
};

struct DetectedInput {
    std::string camera_id;
    float left, top, right, bottom;
    float cx, cy;
    float confidence = 1.0f;
    AppearanceFeature appearance;
};

struct StableObject {
    std::string stable_id;
    std::string camera_id;
    float left, top, right, bottom;
    float cx, cy;
    float confidence;
    bool  is_recovered = false;
    std::string recovered_from;
};

class IdStabilizer {
public:
    explicit IdStabilizer(const IdStabilizerConfig& cfg = {});
    ~IdStabilizer();

    std::vector<StableObject> update(
        const std::vector<DetectedInput>& detections,
        int64_t timestamp_ms);

    void remove_track(const std::string& stable_id);
    void reset();
    int active_track_count() const;
    int gallery_track_count() const;

private:
    struct Track;
    struct Impl;
    Impl* impl_;
};
