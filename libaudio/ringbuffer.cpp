#include "ringbuffer.hpp"

RingBuffer::RingBuffer(std::size_t capacity)
    : buffer_(capacity),
      capacity_(capacity),
      readPos_(0),
      writePos_(0),
      size_(0)
{
}

std::size_t RingBuffer::write(const uint8_t* data, std::size_t len)
{
    if (!data || len == 0) return 0;

    std::unique_lock<std::mutex> lock(mutex_);

    std::size_t written = 0;
    while (written < len) {
        while (size_ == capacity_) {
            notFull_.wait(lock);
        }
        buffer_[writePos_] = data[written];
        writePos_ = (writePos_ + 1) % capacity_;
        ++size_;
        ++written;
    }

    return written;
}

std::size_t RingBuffer::read(uint8_t* out, std::size_t len)
{
    if (!out || len == 0) return 0;

    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t actual = (len < size_) ? len : size_;
    for (std::size_t i = 0; i < actual; ++i) {
        out[i] = buffer_[readPos_];
        readPos_ = (readPos_ + 1) % capacity_;
    }
    size_ -= actual;

    if (actual > 0) {
        notFull_.notify_one();
    }

    return actual;
}

std::size_t RingBuffer::availableData()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

std::size_t RingBuffer::availableSpace()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_ - size_;
}

void RingBuffer::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    readPos_ = 0;
    writePos_ = 0;
    size_ = 0;
}
