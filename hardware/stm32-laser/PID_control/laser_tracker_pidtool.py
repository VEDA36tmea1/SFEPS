#!/usr/bin/env python3
"""
Laser tracker helper script for PID tuning.

- 기반 코드: 사용자가 제공한 LaserTracker 예제
- 변경 사항:
  - 카메라 장치 번호뿐 아니라 동영상 파일/RTSP URL도 입력 소스로 사용 가능
  - OpenCV 3/4 기준 API로 정리

사용 예:

  # 기본 웹캠(0번) 사용
  python3 laser_tracker_pidtool.py

  # 특정 카메라 인덱스
  python3 laser_tracker_pidtool.py --source 1

  # RTSP / 동영상 파일
  python3 laser_tracker_pidtool.py --source rtsp://user:pass@ip/stream
  python3 laser_tracker_pidtool.py --source ./some_video.mp4
"""

import sys
import argparse

import cv2
import numpy


class LaserTracker(object):
    def __init__(
        self,
        cam_width=640,
        cam_height=480,
        hue_min=20,
        hue_max=160,
        sat_min=100,
        sat_max=255,
        val_min=200,
        val_max=255,
        display_thresholds=False,
    ):
        self.cam_width = cam_width
        self.cam_height = cam_height
        self.hue_min = hue_min
        self.hue_max = hue_max
        self.sat_min = sat_min
        self.sat_max = sat_max
        self.val_min = val_min
        self.val_max = val_max
        self.display_thresholds = display_thresholds

        self.capture = None  # camera/video capture device
        self.channels = {
            "hue": None,
            "saturation": None,
            "value": None,
            "laser": None,
        }

        self.previous_position = None
        self.trail = numpy.zeros((self.cam_height, self.cam_width, 3), numpy.uint8)

    def create_and_position_window(self, name, xpos, ypos):
        cv2.namedWindow(name)
        cv2.resizeWindow(name, self.cam_width, self.cam_height)
        cv2.moveWindow(name, xpos, ypos)

    def setup_camera_capture(self, source="0"):
        """
        source:
          - "0", "1", ...  → 카메라 인덱스로 해석
          - 그 외 문자열  → 파일 경로 또는 RTSP/HTTP URL 로 사용
        """
        try:
            # 정수로 파싱되면 카메라 인덱스로 사용
            device = int(source)
            sys.stdout.write(f"Using Camera Device: {device}\n")
            cap = cv2.VideoCapture(device)
        except (ValueError, TypeError):
            sys.stdout.write(f"Using Video Source: {source}\n")
            cap = cv2.VideoCapture(source)

        if not cap.isOpened():
            sys.stderr.write("Failed to open capture source. Quitting.\n")
            sys.exit(1)

        # 카메라일 경우에만 해상도 설정을 시도 (파일/RTSP는 무시될 수 있음)
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.cam_width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.cam_height)

        self.capture = cap
        return self.capture

    def handle_quit(self, delay=10):
        key = cv2.waitKey(delay)
        if key == -1:
            return
        c = chr(key & 255)
        if c in ["c", "C"]:
            self.trail = numpy.zeros((self.cam_height, self.cam_width, 3), numpy.uint8)
        if c in ["q", "Q", chr(27)]:
            sys.exit(0)

    def threshold_image(self, channel):
        if channel == "hue":
            minimum = self.hue_min
            maximum = self.hue_max
        elif channel == "saturation":
            minimum = self.sat_min
            maximum = self.sat_max
        elif channel == "value":
            minimum = self.val_min
            maximum = self.val_max
        else:
            return

        # 상한값 기준으로 TOZERO_INV
        (_, tmp) = cv2.threshold(
            self.channels[channel],
            maximum,
            0,
            cv2.THRESH_TOZERO_INV,
        )

        # 하한값 기준으로 BINARY
        (_, self.channels[channel]) = cv2.threshold(
            tmp,
            minimum,
            255,
            cv2.THRESH_BINARY,
        )

        if channel == "hue":
            # 빨간색 계열은 Hue 범위가 split 되는 특성이 있어서 NOT 사용
            self.channels["hue"] = cv2.bitwise_not(self.channels["hue"])

    def track(self, frame, mask):
        center = None

        contours = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)[-2]

        if len(contours) > 0:
            c = max(contours, key=cv2.contourArea)
            ((x, y), radius) = cv2.minEnclosingCircle(c)
            moments = cv2.moments(c)
            if moments["m00"] > 0:
                center = (
                    int(moments["m10"] / moments["m00"]),
                    int(moments["m01"] / moments["m00"]),
                )
            else:
                center = int(x), int(y)

            if radius > 5:
                cv2.circle(frame, (int(x), int(y)), int(radius), (0, 255, 255), 2)
                cv2.circle(frame, center, 5, (0, 0, 255), -1)
                if self.previous_position:
                    cv2.line(
                        self.trail,
                        self.previous_position,
                        center,
                        (255, 255, 255),
                        2,
                    )

        # frame 크기가 trail과 다르면 trail 크기에 맞게 리사이즈
        if frame.shape[:2] != self.trail.shape[:2]:
            frame = cv2.resize(frame, (self.trail.shape[1], self.trail.shape[0]))

        cv2.add(self.trail, frame, frame)
        self.previous_position = center

    def detect(self, frame):
        hsv_img = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)

        h, s, v = cv2.split(hsv_img)
        self.channels["hue"] = h
        self.channels["saturation"] = s
        self.channels["value"] = v

        self.threshold_image("hue")
        self.threshold_image("saturation")
        self.threshold_image("value")

        # HSV AND 연산으로 레이저 위치 추출
        self.channels["laser"] = cv2.bitwise_and(
            self.channels["hue"], self.channels["value"]
        )
        self.channels["laser"] = cv2.bitwise_and(
            self.channels["saturation"], self.channels["laser"]
        )

        hsv_image = cv2.merge(
            [
                self.channels["hue"],
                self.channels["saturation"],
                self.channels["value"],
            ]
        )

        self.track(frame, self.channels["laser"])

        return hsv_image

    def display(self, img, frame):
        cv2.imshow("RGB_VideoFrame", frame)
        cv2.imshow("LaserPointer", self.channels["laser"])
        if self.display_thresholds:
            cv2.imshow("Thresholded_HSV_Image", img)
            cv2.imshow("Hue", self.channels["hue"])
            cv2.imshow("Saturation", self.channels["saturation"])
            cv2.imshow("Value", self.channels["value"])

    def setup_windows(self):
        sys.stdout.write(f"Using OpenCV version: {cv2.__version__}\n")
        self.create_and_position_window("LaserPointer", 0, 0)
        self.create_and_position_window("RGB_VideoFrame", 10 + self.cam_width, 0)
        if self.display_thresholds:
            self.create_and_position_window("Thresholded_HSV_Image", 10, 10)
            self.create_and_position_window("Hue", 20, 20)
            self.create_and_position_window("Saturation", 30, 30)
            self.create_and_position_window("Value", 40, 40)

    def run(self, source="0"):
        self.setup_windows()
        self.setup_camera_capture(source)

        # 첫 프레임 크기에 맞춰 trail 버퍼를 초기화 (RTSP/파일 해상도 대응)
        success, frame = self.capture.read()
        if not success or frame is None:
            sys.stderr.write("Could not read first frame from source. Quitting.\n")
            sys.exit(1)

        h0, w0 = frame.shape[:2]
        self.cam_width = w0
        self.cam_height = h0
        self.trail = numpy.zeros((self.cam_height, self.cam_width, 3), numpy.uint8)

        # 첫 프레임도 처리
        while True:
            if frame is None:
                success, frame = self.capture.read()
                if not success or frame is None:
                    sys.stderr.write("Could not read frame from source. Quitting.\n")
                    sys.exit(1)

            hsv_image = self.detect(frame)
            self.display(hsv_image, frame)
            self.handle_quit()

            # 다음 루프를 위해 새 프레임 읽기
            success, frame = self.capture.read()
            if not success:
                frame = None


def main():
    parser = argparse.ArgumentParser(description="Run the Laser Tracker for PID tuning")
    parser.add_argument(
        "--source",
        "-S",
        default="0",
        help="카메라 인덱스(0/1/2...) 또는 동영상/RTSP URL (기본: 0)",
    )
    parser.add_argument("--width", "-W", default=640, type=int, help="Camera width")
    parser.add_argument("--height", "-H", default=480, type=int, help="Camera height")
    parser.add_argument(
        "--huemin", "-u", default=20, type=int, help="Hue minimum threshold"
    )
    parser.add_argument(
        "--huemax", "-U", default=160, type=int, help="Hue maximum threshold"
    )
    parser.add_argument(
        "--satmin", "-s", default=100, type=int, help="Saturation minimum threshold"
    )
    parser.add_argument(
        "--satmax", "-Smax", default=255, type=int, help="Saturation maximum threshold"
    )
    parser.add_argument(
        "--valmin", "-v", default=200, type=int, help="Value minimum threshold"
    )
    parser.add_argument(
        "--valmax", "-V", default=255, type=int, help="Value maximum threshold"
    )
    parser.add_argument(
        "--display",
        "-d",
        action="store_true",
        help="Display extra threshold windows",
    )
    args = parser.parse_args()

    tracker = LaserTracker(
        cam_width=args.width,
        cam_height=args.height,
        hue_min=args.huemin,
        hue_max=args.huemax,
        sat_min=args.satmin,
        sat_max=args.satmax,
        val_min=args.valmin,
        val_max=args.valmax,
        display_thresholds=args.display,
    )
    tracker.run(args.source)


if __name__ == "__main__":
    main()

