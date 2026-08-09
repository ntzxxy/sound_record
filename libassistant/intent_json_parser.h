#ifndef INTENT_JSON_PARSER_H
#define INTENT_JSON_PARSER_H

#include "assistant_types.h"

#include <string>

namespace assistant {

class IntentJsonParser {
public:
    bool parse(const std::string& text, IntentResult* result, std::string* error) const;
};

}  // namespace assistant

#endif  // INTENT_JSON_PARSER_H
