#ifndef ASSISTANT_EVENT_LOG_H
#define ASSISTANT_EVENT_LOG_H

#include "assistant_types.h"

#include <mutex>
#include <string>
#include <vector>

namespace assistant {

class EventLog {
public:
    explicit EventLog(std::string storage_path);

    bool load();
    bool append(const DeviceEvent& event);
    bool removeExact(const DeviceEvent& event);
    bool save() const;
    std::vector<DeviceEvent> snapshot() const;

private:
    std::string storage_path_;
    std::vector<DeviceEvent> events_;
    mutable std::mutex mutex_;
};

}  // namespace assistant

#endif  // ASSISTANT_EVENT_LOG_H
