#pragma once
#include <string>
#include <vector>
#include <map>

// 데이터를 담을 구조체 정의
struct DetectedObject {
    std::string id;
    std::string type;
    float x;
    float y;
};

class XMLParser {
private:
    std::map<std::string, unsigned int> log_timer_map;
    std::map<std::string, unsigned int> gate_last_pass_time;
    const unsigned int LOG_THROTTLE = 90000; 
    const unsigned int TAILGATE_LIMIT = 45000;
    const float SENSOR_WIDTH = 1.0f;  // 필요시 수정
    const float SENSOR_HEIGHT = 1.0f; // 필요시 수정

    std::string get_current_time_str();

public:
    // 리턴 타입을 void -> std::vector<DetectedObject> 로 변경
    std::vector<DetectedObject> parseAndProcess(std::string& accumulated_xml, unsigned int last_timestamp);
};