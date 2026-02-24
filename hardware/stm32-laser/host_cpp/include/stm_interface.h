#pragma once

#include <string>

class StmInterface
{
public:
    // serial_device: 예) "/dev/ttyUSB0"
    // baudrate     : 예) 115200
    StmInterface(const std::string& serial_device, int baudrate);
    ~StmInterface();

    // 유효한지(포트 오픈 성공했는지) 여부
    bool isOpen() const;

    // PAN/TILT PWM(us) 값을 "u1 u2\r\n" 형식으로 STM으로 전송
    void sendPwm(int pan_us, int tilt_us);

    // 필요 시 manual 모드 강제 설정용
    void sendModeManual();

private:
    int fd_{-1};
};

