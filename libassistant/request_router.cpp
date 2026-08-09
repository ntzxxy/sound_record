#include "request_router.h"

#include <cctype>

namespace assistant {
namespace {

bool isBlank(const std::string& input) {
    for (unsigned char c : input) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

}  // namespace

RequestAnalysis RequestRouter::analyze(const std::string& input) const {
    RequestAnalysis analysis;
    if (input.empty() || isBlank(input)) {
        analysis.task_type = IntentType::Clarify;
        analysis.matched_rule = "empty_input";
        return analysis;
    }

    analysis.task_type = IntentType::GeneralChat;
    analysis.matched_rule = "rule_baseline_disabled_semantic_router_primary";
    return analysis;
}

}  // namespace assistant
