#ifndef CONTEXT_BUILDER_H
#define CONTEXT_BUILDER_H

#include "memory_store.h"

#include <cstddef>
#include <string>

namespace assistant {

class ContextBuilder {
public:
    std::string buildMemoryContext(const MemoryQuery& query,
                                   const MemoryStore& store,
                                   std::size_t max_items = 5,
                                   std::size_t max_chars = 600) const;
};

}  // namespace assistant

#endif  // CONTEXT_BUILDER_H
