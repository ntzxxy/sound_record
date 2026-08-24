#include "context_builder.h"

namespace assistant {
namespace {

std::string describeMemory(const MemoryItem& item) {
    if (item.category == "USER_PREFERENCE") {
        return "- 用户偏好：" + item.subject + "的" + item.attribute + "是" + item.value + "。";
    }
    if (item.category == "OBJECT_LOCATION") {
        return "- 物品位置：" + item.subject + "位于" + item.value + "。";
    }
    return "- 系统记忆：" + item.subject + "的" + item.attribute + "是" + item.value + "。";
}

}  // namespace

std::string ContextBuilder::buildMemoryContext(const MemoryQuery& query,
                                               const MemoryStore& store,
                                               std::size_t max_items,
                                               std::size_t max_chars) const {
    const auto items = store.selectRelevant(query, max_items);
    if (items.empty()) return "";

    std::string context = "【相关系统记忆】\n";
    for (const auto& item : items) {
        std::string line = describeMemory(item);
        if (context.size() + line.size() + 1 > max_chars) break;
        context += line;
        context += '\n';
    }
    if (context == "【相关系统记忆】\n") return "";
    return context;
}

}  // namespace assistant
