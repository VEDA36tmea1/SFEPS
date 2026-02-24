#pragma once

#include <vector>
#include <mutex>
#include <condition_variable>
#include <cstddef>

// 간단한 멀티스레드용 오디오 링 버퍼 (바이트 단위, 프레임 크기는 호출자가 관리)
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(std::size_t capacity_bytes);

    // data[0..bytes) 를 버퍼에 push.
    // 버퍼가 꽉 차면 공간이 생길 때까지 block.
    void push(const char* data, std::size_t bytes);

    // 최대 max_bytes 만큼 pop 하여 out 에 채움.
    // 실제로 읽은 바이트 수를 반환. (데이터가 없으면 block)
    std::size_t pop(char* out, std::size_t max_bytes);

    // 종료 플래그 설정: 대기 중인 pop/push를 깨우기 위해 사용.
    void stop();

    bool stopped() const;

private:
    std::vector<char> buffer_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;

    mutable std::mutex mtx_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    bool stopped_ = false;
};
