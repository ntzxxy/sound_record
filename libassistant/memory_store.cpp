#include "memory_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace assistant {
namespace {

constexpr std::size_t kMaxMemoryItems = 50;

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

int relevanceScore(const MemoryItem& item, const MemoryQuery& query) {
    int score = 0;
    if (!query.subject.empty() && item.subject == query.subject) score += 8;
    else if (!query.subject.empty() && item.subject.find(query.subject) != std::string::npos) score += 4;
    else if (!query.subject.empty() && query.subject.find(item.subject) != std::string::npos) score += 3;

    if (!query.attribute.empty() && item.attribute == query.attribute) score += 5;
    else if (!query.attribute.empty() && item.attribute.find(query.attribute) != std::string::npos) score += 2;
    return score;
}

}  // namespace

MemoryStore::MemoryStore(std::string storage_path)
    : storage_path_(std::move(storage_path)) {}

bool MemoryStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();

    std::ifstream in(storage_path_);
    if (!in.is_open()) return true;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto fields = splitTsv(line);
        if (fields.size() < 5) continue;

        MemoryItem item;
        item.category = fields[0];
        item.subject = fields[1];
        item.attribute = fields[2];
        item.value = fields[3];
        try {
            item.updated_at = std::stoll(fields[4]);
        } catch (...) {
            item.updated_at = 0;
        }
        if (!item.category.empty() && !item.subject.empty() &&
            !item.attribute.empty() && !item.value.empty()) {
            items_.push_back(std::move(item));
        }
    }

    if (!in.good() && !in.eof()) {
        items_.clear();
        return false;
    }
    if (items_.size() > kMaxMemoryItems) {
        std::sort(items_.begin(), items_.end(), [](const MemoryItem& a, const MemoryItem& b) {
            return a.updated_at < b.updated_at;
        });
        items_.erase(items_.begin(), items_.end() - static_cast<std::ptrdiff_t>(kMaxMemoryItems));
    }
    return true;
}

bool MemoryStore::save() const {
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
        for (const auto& item : items_) {
            out << escapeField(item.category) << '\t'
                << escapeField(item.subject) << '\t'
                << escapeField(item.attribute) << '\t'
                << escapeField(item.value) << '\t'
                << item.updated_at << '\n';
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

void MemoryStore::upsert(const MemoryItem& item) {
    if (item.category.empty() || item.subject.empty() || item.attribute.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& existing : items_) {
        if (existing.category == item.category &&
            existing.subject == item.subject &&
            existing.attribute == item.attribute) {
            existing = item;
            return;
        }
    }
    items_.push_back(item);
    if (items_.size() > kMaxMemoryItems) {
        auto oldest = std::min_element(items_.begin(), items_.end(),
            [](const MemoryItem& a, const MemoryItem& b) {
                return a.updated_at < b.updated_at;
            });
        if (oldest != items_.end()) items_.erase(oldest);
    }
}

std::size_t MemoryStore::removeMatching(const MemoryDeleteRequest& request) {
    if (request.delete_all) return clear();
    if (request.subject.empty() && request.category.empty()) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto old_size = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
        [&request](const MemoryItem& item) {
            if (!request.category.empty() && item.category != request.category) return false;
            if (request.subject.empty()) return true;
            return item.subject == request.subject ||
                   item.subject.find(request.subject) != std::string::npos ||
                   request.subject.find(item.subject) != std::string::npos;
        }),
        items_.end());
    return old_size - items_.size();
}

std::size_t MemoryStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto old_size = items_.size();
    items_.clear();
    return old_size;
}

std::vector<MemoryItem> MemoryStore::selectRelevant(const MemoryQuery& query,
                                                    std::size_t max_items) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<int, MemoryItem>> scored;
    for (const auto& item : items_) {
        int score = relevanceScore(item, query);
        if (score > 0) scored.push_back({score, item});
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second.updated_at > b.second.updated_at;
    });

    std::vector<MemoryItem> result;
    for (const auto& item : scored) {
        if (result.size() >= max_items) break;
        result.push_back(item.second);
    }
    return result;
}

std::vector<MemoryItem> MemoryStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

}  // namespace assistant
