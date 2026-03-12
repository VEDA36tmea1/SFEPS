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

    const unsigned int LOG_THROTTLE = 90000; 
    const unsigned int TAILGATE_LIMIT = 45000;
    const float SENSOR_WIDTH = 3840.0f;  // 🌟 가벽(Clamping)용 4K 너비
    const float SENSOR_HEIGHT = 2160.0f; // 🌟 가벽(Clamping)용 4K 높이

    std::string get_current_time_str();

public:
    std::vector<DetectedObject> parseAndProcess(std::string& accumulated_xml, unsigned int last_timestamp);
    std::vector<ParsedMetadataObject> parseHumanObjectsForAnalytics(const std::string& xml) const;
};