#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** BGR 버퍼에 레이저 탐지를 수행하고, 탐지된 점에 빨간 원을 그립니다.
 *  in-place 수정. data는 row_stride 바이트씩 진행하는 BGR 이미지. */
void laserdetect_process(unsigned char* data, int width, int height, int row_stride);

#ifdef __cplusplus
}
#endif
