#ifndef REQUEST_ROUTER_H
#define REQUEST_ROUTER_H

#include "assistant_types.h"

#include <string>

namespace assistant {

class RequestRouter {
public:
    RequestAnalysis analyze(const std::string& input) const;
};

}  // namespace assistant

#endif  // REQUEST_ROUTER_H
