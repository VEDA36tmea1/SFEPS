import spidev
import RPi.GPIO as GPIO

from mfrc522 import SimpleMFRC522

spi = spidev.SpiDev()
spi.open(0, 0) # /dev/spidev0.0 열기

print(f"SPI 모드: {spi.mode}")
print(f"최대 속도: {spi.max_speed_hz} Hz")
print(f"비트당 비트 수: {spi.bits_per_word}")
print(f"CS 활성 수준 (Low/High): {spi.cshigh}")

spi.close()

reader = SimpleMFRC522()

try:
        id, text = reader.read()
        print(id)
        print(text)
finally:
        GPIO.cleanup()

# # 핀 설정 (BCM 기준)
# RST_PIN = 25  # 사용자 연결 기준 (Pin 22)

# # SPI 초기화
# spi = spidev.SpiDev()
# spi.open(0, 0)  # Bus 0, Device 0 (CE0 / Pin 24)
# spi.max_speed_hz = 1000000 # 1MHz 속도

# def read_rc522_version():
#     # 리셋 핀 초기화 (모듈 깨우기)
#     GPIO.setmode(GPIO.BCM)
#     GPIO.setup(RST_PIN, GPIO.OUT)
#     GPIO.output(RST_PIN, GPIO.HIGH) # Reset 해제

#     # RC522 Version Register 주소: 0x37
#     # 읽기 모드 비트(0x80) 설정 -> ((0x37 << 1) & 0x7E) | 0x80
#     addr = ((0x37 << 1) & 0x7E) | 0x80
    
#     # [주소, 더미 데이터] 전송 후 수신
#     response = spi.xfer2([addr, 0x00])
    
#     # response[1]에 버전 정보가 담겨 옴
#     return response[1]

# try:
#     version = read_rc522_version()
#     print(f"RC522 Version Check: 0x{version:02X}")
    
#     if version == 0x91 or version == 0x92:
#         print("✅ 통신 성공! 모듈이 정상적으로 인식되었습니다.")
#     elif version == 0x00 or version == 0xFF:
#         print("❌ 통신 실패: 배선을 다시 확인하세요. (0x00 또는 0xFF)")
#     else:
#         print("⚠️ 알 수 없는 버전: 모듈이 다르거나 연결이 불안정합니다.")

# finally:
#     spi.close()
#     GPIO.cleanup()