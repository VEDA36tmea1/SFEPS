#include "ibvs_controller.h"

#include <algorithm>

IbvsController::IbvsController(int pwm_min_us, int pwm_max_us, int neutral_us,
                               double Ku, double Kv)
    : pwm_min_(pwm_min_us),
      pwm_max_(pwm_max_us),
      neutral_(neutral_us),
      Ku_(Ku),
      Kv_(Kv),
      // STM32 초기 PWM(Servo_Init)과 맞추기 위해 PAN/TILT 시작값을 하드코딩
      // PA8(TIM1_CH1) ≈ 1250us, PA0(TIM2_CH1) ≈ 1430us
      current_pan_us_(1430.0),
      current_tilt_us_(1250.0)
{
}

IbvsOutput IbvsController::update(double e_u, double e_v, double /*dt_sec*/)
{
    // 단순 P 제어: current += K * error
    current_pan_us_  += Ku_ * e_u;
    current_tilt_us_ += Kv_ * e_v;

    int pan_clamped  = clampPwm(static_cast<int>(current_pan_us_));
    int tilt_clamped = clampPwm(static_cast<int>(current_tilt_us_));

    current_pan_us_  = pan_clamped;
    current_tilt_us_ = tilt_clamped;

    IbvsOutput out;
    out.pan_us  = pan_clamped;
    out.tilt_us = tilt_clamped;
    return out;
}

int IbvsController::clampPwm(int us) const
{
    return std::max(pwm_min_, std::min(pwm_max_, us));
}

