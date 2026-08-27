#ifndef MEMORY_STORE_H
#define MEMORY_STORE_H

#include "assistant_types.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace assistant {

class MemoryStore {
public:
    explicit MemoryStore(std::string storage_path);

    bool load();
    bool save() const;
    void upsert(const MemoryItem& item);
    std::size_t removeMatching(const MemoryDeleteRequest& request);
    bool removeExact(const MemoryItem& item);
    std::size_t clear();

    std::vector<MemoryItem> selectRelevant(const MemoryQuery& query,
                                           std::size_t max_items = 5) const;
    std::vector<MemoryItem> snapshot() const;

private:
    std::string storage_path_;
    std::vector<MemoryItem> items_;
    mutable std::mutex mutex_;
};

}  // namespace assistant

#endif  // MEMORY_STORE_H
