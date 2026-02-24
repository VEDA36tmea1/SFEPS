#include "audio_ring_buffer.h"
#include <cstring>

AudioRingBuffer::AudioRingBuffer(std::size_t capacity_bytes)
    : buffer_(capacity_bytes)
{
}

void AudioRingBuffer::push(const char* data, std::size_t bytes)
{
    std::unique_lock<std::mutex> lock(mtx_);
    while (!stopped_ && bytes > 0) {
        // 버퍼가 꽉 찼으면 공간 생길 때까지 대기
        cv_not_full_.wait(lock, [this] { return stopped_ || size_ < buffer_.size(); });
        if (stopped_) break;

        std::size_t space = buffer_.size() - size_;
        if (space == 0) continue;

        std::size_t chunk = (bytes < space) ? bytes : space;

        // tail_ 위치부터 chunk 만큼 순환하며 복사
        std::size_t tail_to_end = buffer_.size() - tail_;
        if (chunk <= tail_to_end) {
            std::memcpy(&buffer_[tail_], data, chunk);
            tail_ = (tail_ + chunk) % buffer_.size();
        } else {
            std::memcpy(&buffer_[tail_], data, tail_to_end);
            std::memcpy(&buffer_[0], data + tail_to_end, chunk - tail_to_end);
            tail_ = chunk - tail_to_end;
        }

        size_ += chunk;
        data  += chunk;
        bytes -= chunk;

        cv_not_empty_.notify_one();
    }
}

std::size_t AudioRingBuffer::pop(char* out, std::size_t max_bytes)
{
    std::unique_lock<std::mutex> lock(mtx_);
    // 데이터가 없고, 아직 stopped_ 가 아니면 대기
    cv_not_empty_.wait(lock, [this] { return stopped_ || size_ > 0; });
    if (size_ == 0) return 0; // stopped_ 이면서 비었을 때

    std::size_t available = size_;
    std::size_t chunk = (max_bytes < available) ? max_bytes : available;

    std::size_t head_to_end = buffer_.size() - head_;
    if (chunk <= head_to_end) {
        std::memcpy(out, &buffer_[head_], chunk);
        head_ = (head_ + chunk) % buffer_.size();
    } else {
        std::memcpy(out, &buffer_[head_], head_to_end);
        std::memcpy(out + head_to_end, &buffer_[0], chunk - head_to_end);
        head_ = chunk - head_to_end;
    }

    size_ -= chunk;
    cv_not_full_.notify_one();
    return chunk;
}

void AudioRingBuffer::stop()
{
    std::lock_guard<std::mutex> lock(mtx_);
    stopped_ = true;
    cv_not_empty_.notify_all();
    cv_not_full_.notify_all();
}

bool AudioRingBuffer::stopped() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return stopped_;
}
