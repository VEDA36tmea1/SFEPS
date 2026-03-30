#pragma once
#include <opencv2/opencv.hpp>
#include <cstdint>
 
// ── RAW ISP 설정 ──────────────────────────────────────────────
struct ISPConfig {
    uint16_t black_level = 64; // 센서 블랙 레벨 (10-bit 기준)
};
 
// ── 공개 인터페이스 ───────────────────────────────────────────
 
/**
 * runPureISP
 *  입력: CV_16UC1  RAW 10-bit 베이어 프레임 (원본 보존)
 *  출력: CV_8UC3   BGR 이미지
 *  처리: BLC → AWB/AE → Demosaic → CCM → Gamma
 */
cv::Mat runPureISP(const cv::Mat& raw16_frame);
 
/**
 * processISPAndGetBest
 *  입력: CV_8UC3   BGR 이미지 (runPureISP 결과 or libcamera BGR 직접 입력)
 *  출력: best_frame      — 엔트로피 최고 후보
 *        tuning_view_out — 8분할 비교 뷰 (4x2 그리드)
 *  처리: ShadowBoost × 7 → CLAHE × 7 → Entropy 평가 → Best 선택
 */
cv::Mat processISPAndGetBest(const cv::Mat& frame_in, cv::Mat& tuning_view_out);
cv::Mat runPureISP_withHistograms(const cv::Mat& raw16_frame, const std::string& save_dir);

// ── 내부 공유 함수 (단독 사용 가능) ─────────────────────────
void applyShadowBoost(const cv::Mat& src, cv::Mat& dst, double gamma, double alpha);
void applyCLAHE(const cv::Mat& src, cv::Mat& dst, double clip_limit, cv::Size grid);
 