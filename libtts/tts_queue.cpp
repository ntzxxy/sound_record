#include "tts_queue.h"

void DoubleMessageQueue::push_text(const std::string &msg, bool is_final)
{

    std::lock_guard<std::mutex> lock(text_mutex_);
    text_queue_.push(TextMessage{msg, is_final, false});
    text_cond_.notify_one();
}

TextMessage DoubleMessageQueue::pop_text()
{
    std::unique_lock<std::mutex> lock(text_mutex_);
    text_cond_.wait(lock, [this]
                    { return !text_queue_.empty() || stop_; });

    if (stop_)
        return TextMessage{"", true, true};

    TextMessage msg = std::move(text_queue_.front());
    text_queue_.pop();
    return msg;
}

void DoubleMessageQueue::push_audio(std::unique_ptr<int16_t[]> data, size_t length, bool is_last)
{
    AudioMessage msg{std::move(data), length, is_last};
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_queue_.push(std::move(msg));
    }
    audio_cond_.notify_one();
}

AudioMessage DoubleMessageQueue::pop_audio()
{
    std::unique_lock<std::mutex> lock(audio_mutex_);
    audio_cond_.wait(lock, [this]
                     { return !audio_queue_.empty() || stop_; });

    if (stop_)
        return {nullptr, 0, true};

    AudioMessage msg = std::move(audio_queue_.front());
    audio_queue_.pop();
    return msg;
}

void DoubleMessageQueue::clear()
{
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        std::queue<TextMessage> empty;
        text_queue_.swap(empty);
    }
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        std::queue<AudioMessage> empty;
        audio_queue_.swap(empty);
    }
}

void DoubleMessageQueue::stop()
{
    {
        std::lock_guard<std::mutex> lock1(text_mutex_);
        std::lock_guard<std::mutex> lock2(audio_mutex_);
        stop_ = true;
    }
    text_cond_.notify_all();
    audio_cond_.notify_all();
}
