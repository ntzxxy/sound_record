#ifndef REQUEST_ROUTER_H
#define REQUEST_ROUTER_H

#include "assistant_types.h"
#include "semantic_intent_router.h"

#include <memory>
#include <string>

namespace assistant {

class RequestRouter {
public:
    explicit RequestRouter(
        std::shared_ptr<const SemanticIntentRouter> semantic_router =
            createSemanticIntentRouterFromEnvironment());
    RequestAnalysis analyze(const std::string& input) const;

private:
    std::shared_ptr<const SemanticIntentRouter> semantic_router_;
};

}  // namespace assistant

#endif  // REQUEST_ROUTER_H
