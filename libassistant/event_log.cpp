#include "event_log.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <utility>

namespace assistant {
namespace {

std::string escapeField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string unescapeField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            out.push_back(value[i]);
            continue;
        }
        const char n = value[++i];
        switch (n) {
            case 't': out.push_back('\t'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case '\\': out.push_back('\\'); break;
            default:
                out.push_back('\\');
                out.push_back(n);
                break;
        }
    }
    return out;
}

std::vector<std::string> splitTsv(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) tab = line.size();
        fields.push_back(unescapeField(line.substr(start, tab - start)));
        if (tab == line.size()) break;
        start = tab + 1;
    }
    return fields;
}

}  // namespace

EventLog::EventLog(std::string storage_path)
    : storage_path_(std::move(storage_path)) {}

bool EventLog::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();

    std::ifstream in(storage_path_);
    if (!in.is_open()) return true;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto fields = splitTsv(line);
        if (fields.size() < 5) continue;

        DeviceEvent event;
        try {
            event.timestamp = std::stoll(fields[0]);
        } catch (...) {
            event.timestamp = 0;
        }
        event.room = fields[1];
        event.device = fields[2];
        event.event_type = fields[3];
        event.description = fields[4];
        if (!event.device.empty() && !event.description.empty()) {
            events_.push_back(std::move(event));
        }
    }

    if (!in.good() && !in.eof()) {
        events_.clear();
        return false;
    }
    return true;
}

bool EventLog::append(const DeviceEvent& event) {
    if (event.device.empty() || event.description.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
    }
    return save();
}

bool EventLog::removeExact(const DeviceEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(events_.begin(), events_.end(), [&](const DeviceEvent& existing) {
        return existing.timestamp == event.timestamp &&
               existing.room == event.room &&
               existing.device == event.device &&
               existing.event_type == event.event_type &&
               existing.description == event.description;
    });
    if (found == events_.end()) return false;
    events_.erase(found);
    return true;
}

bool EventLog::save() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::filesystem::path path(storage_path_);
    const std::filesystem::path dir = path.parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return false;
    }

    const std::filesystem::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
        if (!out.is_open()) return false;
        for (const auto& event : events_) {
            out << event.timestamp << '\t'
                << escapeField(event.room) << '\t'
                << escapeField(event.device) << '\t'
                << escapeField(event.event_type) << '\t'
                << escapeField(event.description) << '\n';
        }
        out.flush();
        if (!out.good()) return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path);
        return false;
    }
    return true;
}

std::vector<DeviceEvent> EventLog::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

}  // namespace assistant
