#pragma once

#include <utility>

struct IbvsOutput
{
    int pan_us{1500};
    int tilt_us{1500};
};

class IbvsController
{
public:
    IbvsController(int pwm_min_us, int pwm_max_us, int neutral_us,
                   double Ku, double Kv);

    // e_u, e_v: 타겟 - 레이저 픽셀 오차
    // dt_sec  : 이전 업데이트 이후 경과 시간(초) – 현재 구현은 P 제어 기준으로 사용 안 할 수도 있음
    IbvsOutput update(double e_u, double e_v, double dt_sec);

private:
    int clampPwm(int us) const;

    int pwm_min_;
    int pwm_max_;
    int neutral_;

    double Ku_;  // [us / pixel] for pan
    double Kv_;  // [us / pixel] for tilt

    double current_pan_us_;
    double current_tilt_us_;
};

