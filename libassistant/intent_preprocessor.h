#ifndef INTENT_PREPROCESSOR_H
#define INTENT_PREPROCESSOR_H

#include "assistant_types.h"
#include "intent_json_parser.h"

#include <string>

namespace assistant {

class IntentPreprocessor {
public:
    bool initialize();
    // semantic_hint is a locally detected, untrusted candidate category.  It
    // selects stricter extraction guidance but never supplies user facts.
    IntentResult analyze(const std::string& user_input,
                         const std::string& semantic_hint = "");

private:
    IntentJsonParser parser_;
};

}  // namespace assistant

#endif  // INTENT_PREPROCESSOR_H
