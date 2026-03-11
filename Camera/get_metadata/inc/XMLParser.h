#pragma once
#include <string>
#include <map>

class XMLParser {
private:
    std::map<std::string, unsigned int> log_timer_map;       // 로그 스로틀링용
    std::map<std::string, unsigned int> gate_last_pass_time; // 꼬리물기 감지용

    std::string get_current_time_str();

public:
    // 누적된 XML 문자열을 받아 처리하고, 처리된 만큼 지우는 함수
    void parseAndProcess(std::string& accumulated_xml, unsigned int current_timestamp);
};
