#pragma once
#include <string>
#include <vector>
#include <map>
#include <cmath> 

struct DetectedObject {
    std::string id;       
    std::string type;
    float x;
    float y;
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
    const float SENSOR_WIDTH = 1.0f;  
    const float SENSOR_HEIGHT = 1.0f; 

    std::string get_current_time_str();

public:
    std::vector<DetectedObject> parseAndProcess(std::string& accumulated_xml, unsigned int last_timestamp);
    std::vector<ParsedMetadataObject> parseHumanObjectsForAnalytics(const std::string& xml) const;
};
