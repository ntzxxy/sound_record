#ifndef SEMANTIC_INTENT_ROUTER_H
#define SEMANTIC_INTENT_ROUTER_H

#include "assistant_types.h"

#include <memory>
#include <string>

namespace assistant {

// Semantic routing answers only "which business family?".  Implementations
// must reject uncertain input as None; consumers must still parse and validate.
class SemanticIntentRouter {
public:
    virtual ~SemanticIntentRouter() = default;
    virtual SemanticRouteResult classify(const std::string& text) const = 0;
};

// Phase 4 supplies the environment-configured local implementation.  A null
// implementation safely rejects every request when no model is configured.
std::shared_ptr<const SemanticIntentRouter> createSemanticIntentRouterFromEnvironment();

}  // namespace assistant

#endif  // SEMANTIC_INTENT_ROUTER_H
