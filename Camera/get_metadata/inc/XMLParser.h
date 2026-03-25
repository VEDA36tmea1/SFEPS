#pragma once
#include <string>
#include <vector>
#include <map>
#include <cmath> 
#include <algorithm> // std::max, std::min 추가

struct DetectedObject {
    std::string id;       
    std::string type;
    float x;
    float y;
    float likelihood; // 🌟 신뢰도 점수 추가
    float w;          // 🌟 너비 추가
    float h;          // 🌟 높이 추가
};

struct ParsedMetadataObject {
    std::string id;
    std::string type;
    float x;
    float y;
    float left;
    float top;
    float right;
    float bottom;
    float likelihood{1.0f};  // 추가
};

struct Trajectory {
    std::string original_id;
    float last_x;
    float last_y;
    float last_w;     // 🌟 박스 너비 기억
    float last_h;     // 🌟 박스 높이 기억
    float vx;                
    float vy;                
    unsigned int last_rtp;
};

class XMLParser {
private:
    std::map<std::string, unsigned int> log_timer_map;
    std::map<std::string, unsigned int> gate_last_pass_time;
    std::map<std::string, Trajectory> tracking_map; 

    // 이름은 Config.h 의 LOG_THROTTLE / TAILGATE_LIMIT / SENSOR_* 매크로와 겹치면 안 됨(전처리기 파괴).
    static constexpr unsigned int kParserObjectLogIntervalRtp = 90000;
    static constexpr unsigned int kParserTailgateGapRtp = 45000;
    static constexpr float kParserClampWidth = 3840.0f;
    static constexpr float kParserClampHeight = 2160.0f;

    std::string get_current_time_str();

public:
    std::vector<DetectedObject> parseAndProcess(std::string& accumulated_xml, unsigned int last_timestamp);
    // detect_all=true 이면 Human 이외 타입도 모두 반환 (좌표가 유효한 경우)
    // detect_all=false 이면 type == "Human" 인 객체만 반환 (기본 동작).
    // 타입 필터 전에 모든 객체를 모아, 더 큰 박스 안에 거의 통째로 들어간 작은 박스를 타입 무관 제거 후 NMS.
    std::vector<ParsedMetadataObject> parseHumanObjectsForAnalytics(const std::string& xml,
                                                                    bool detect_all = false) const;
};