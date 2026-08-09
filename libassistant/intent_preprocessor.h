#ifndef INTENT_PREPROCESSOR_H
#define INTENT_PREPROCESSOR_H

#include "assistant_types.h"
#include "intent_json_parser.h"

#include <string>

namespace assistant {

class IntentPreprocessor {
public:
    bool initialize();
    IntentResult analyze(const std::string& user_input);

private:
    IntentJsonParser parser_;
};

}  // namespace assistant

#endif  // INTENT_PREPROCESSOR_H
