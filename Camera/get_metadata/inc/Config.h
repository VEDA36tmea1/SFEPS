#pragma once

// 카메라 연결 정보
#define CAMERA_IP "192.168.0.30"
#define CAMERA_PORT 554
#define RTSP_URL "rtsp://192.168.0.30/profile2/media.smp"

// 카메라 센서 해상도 (4K 기준 좌표 정규화용)
#define SENSOR_WIDTH  3840.0f
#define SENSOR_HEIGHT 2160.0f

// 로직 임계값
#define TAILGATE_LIMIT 90000 * 1.5  // 1.5초 (RTP Time 기준)
#define LOG_THROTTLE 270000         // 3초 (로그 출력 제한)
