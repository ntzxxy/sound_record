#ifndef RINGBUFFER_HPP
#define RINGBUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>
#include <condition_variable>

class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity);
    std::size_t write(const uint8_t* data, std::size_t len);
    std::size_t read(uint8_t* out, std::size_t len);
    std::size_t availableData();
    std::size_t availableSpace();
    void reset();



private:
    std::vector<uint8_t> buffer_;
    std::size_t capacity_;
    std::size_t readPos_;
    std::size_t writePos_;
    std::size_t size_;
    std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
};

#endif
