#include "gst_ibvs.h"

#include "ibvs_controller.h"
#include "stm_interface.h"
#include "vision_detector.h"

#include <gst/gst.h>
#include <gst/video/video.h>

#include <iostream>

// GstSample → cv::Mat 변환 (BGR 또는 GRAY 가정)
bool gst_sample_to_mat(GstSample* sample, cv::Mat& outMat)
{
    if (!sample)
        return false;

    GstCaps* caps = gst_sample_get_caps(sample);
    if (!caps)
        return false;

    GstStructure* s = gst_caps_get_structure(caps, 0);
    if (!s)
        return false;

    const gchar* format = gst_structure_get_string(s, "format");
    int width = 0, height = 0;
    if (!gst_structure_get_int(s, "width", &width) ||
        !gst_structure_get_int(s, "height", &height))
    {
        return false;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer)
        return false;

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return false;

    // GStreamer에서 video/x-raw,format=BGR 또는 GRAY를 사용하는 것을 권장.
    int type = CV_8UC3;
    if (format && std::string(format) == "GRAY8")
    {
        type = CV_8UC1;
    }
    // "BGR" 또는 기타 RGB 포맷은 여기서 CV_8UC3으로 처리 (필요 시 확장)

    // 복사 없이 데이터 뷰만 만든다 (라이프타임은 map 해제 전까지만 유효)
    outMat = cv::Mat(height, width, type, const_cast<guint8*>(map.data));

    // caller 에서 즉시 사용 후 buffer unmap 해야 하므로,
    // 여기서는 unmap 하지 않고, 별도 스코프로 관리해도 되지만
    // 간단하게 복사본을 만들어 반환하고 여기서 unmap 처리한다.
    cv::Mat copied = outMat.clone();
    gst_buffer_unmap(buffer, &map);
    outMat = std::move(copied);
    return true;
}

bool process_gst_frame(GstSample* sample,
                       const cv::Point2f& target_center,
                       VisionDetector& detector,
                       IbvsController& controller,
                       StmInterface& stm)
{
    cv::Mat frame;
    if (!gst_sample_to_mat(sample, frame))
    {
        std::cerr << "process_gst_frame: failed to convert sample to cv::Mat\n";
        return false;
    }

    DetectionResult target;
    target.point = target_center;
    target.found = true; // 타겟은 외부 모듈에서 이미 검출된 것으로 가정

    DetectionResult laser = detector.detectLaser(frame);

    if (!(target.found && laser.found))
    {
        std::cerr << "process_gst_frame: detection failed, target_found="
                  << target.found << " laser_found=" << laser.found << "\n";
        return false;
    }

    double e_u = static_cast<double>(target.point.x - laser.point.x);
    double e_v = static_cast<double>(target.point.y - laser.point.y);

    // dt는 상위 레벨에서 관리할 수도 있으나, 여기서는 0으로 넣고
    // 단순 P 제어만 사용하는 것을 가정한다.
    IbvsOutput out = controller.update(e_u, e_v, 0.0);

    if (stm.isOpen())
    {
        stm.sendPwm(out.pan_us, out.tilt_us);
    }

    return true;
}

