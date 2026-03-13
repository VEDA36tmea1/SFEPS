#pragma once

#include <opencv2/core.hpp>

// GStreamer forward declarations
typedef struct _GstSample GstSample;

/** GstSample (video/x-raw, BGR or GRAY8) → cv::Mat. rtsp_laser_demo 등에서 appsink 프레임 변환용. */
bool gst_sample_to_mat(GstSample* sample, cv::Mat& outMat);

class IbvsController;
class StmInterface;
class VisionDetector;

// 외부(객체 검출 모듈)에서 이미 알고 있는 타겟 좌표(target_center)를 입력으로 받고,
// GStreamer 프레임(sample) 안에서 레이저 스폿을 찾아 IBVS 제어/STM 명령까지 수행하는 헬퍼 함수.
//
// - sample       : GStreamer appsink 등에서 받은 GstSample* (video/x-raw, BGR 또는 GRAY 가정)
// - target_center: 객체 검출 모듈이 계산한 타겟 중심 픽셀 좌표
// - detector     : 레이저 검출에 사용하는 VisionDetector
// - controller   : 픽셀 오차를 PWM(us)로 바꾸는 IBVS 컨트롤러
// - stm          : STM 보드로 PWM 값을 보내는 인터페이스
//
// 반환값:
//   true  → 타겟과 레이저 모두 검출되어 제어까지 수행함
//   false → 둘 중 하나라도 실패 (이 경우에는 아무 명령도 보내지 않음)
bool process_gst_frame(GstSample* sample,
                       const cv::Point2f& target_center,
                       VisionDetector& detector,
                       IbvsController& controller,
                       StmInterface& stm);

