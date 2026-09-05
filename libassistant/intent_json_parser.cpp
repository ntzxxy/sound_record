#include "intent_json_parser.h"

#include <cctype>
#include <cstdlib>

namespace assistant {
namespace {

std::string trim(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string extractJsonObject(const std::string& text) {
    const std::size_t begin = text.find('{');
    if (begin == std::string::npos) return "";
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = begin; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) return text.substr(begin, i - begin + 1);
        }
    }
    return "";
}

std::size_t findKey(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\"");
}

std::size_t valueStart(const std::string& json, std::size_t key_pos) {
    if (key_pos == std::string::npos) return std::string::npos;
    const std::size_t colon = json.find(':', key_pos);
    if (colon == std::string::npos) return std::string::npos;
    std::size_t pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    return pos;
}

bool parseJsonStringAt(const std::string& json, std::size_t pos, std::string* out) {
    if (pos >= json.size() || json[pos] != '"') return false;
    std::string value;
    bool escaped = false;
    for (std::size_t i = pos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            switch (c) {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(c); break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            *out = value;
            return true;
        } else {
            value.push_back(c);
        }
    }
    return false;
}

bool getStringField(const std::string& json, const std::string& key, std::string* out) {
    const std::size_t start = valueStart(json, findKey(json, key));
    if (start == std::string::npos) return false;
    return parseJsonStringAt(json, start, out);
}

std::string getObjectField(const std::string& json, const std::string& key, bool* is_null) {
    if (is_null) *is_null = false;
    const std::size_t start = valueStart(json, findKey(json, key));
    if (start == std::string::npos) return "";
    if (json.compare(start, 4, "null") == 0) {
        if (is_null) *is_null = true;
        return "";
    }
    if (json[start] != '{') return "";

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = start; i < json.size(); ++i) {
        const char c = json[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) return json.substr(start, i - start + 1);
        }
    }
    return "";
}

std::optional<double> getOptionalNumberField(const std::string& json, const std::string& key) {
    const std::size_t start = valueStart(json, findKey(json, key));
    if (start == std::string::npos) return std::nullopt;
    if (json.compare(start, 4, "null") == 0) return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(json.c_str() + start, &end);
    if (end == json.c_str() + start) return std::nullopt;
    return value;
}

std::optional<bool> getOptionalBoolField(const std::string& json, const std::string& key) {
    const std::size_t start = valueStart(json, findKey(json, key));
    if (start == std::string::npos) return std::nullopt;
    if (json.compare(start, 4, "true") == 0) return true;
    if (json.compare(start, 5, "false") == 0) return false;
    return std::nullopt;
}

std::vector<std::string> getStringArrayField(const std::string& json, const std::string& key) {
    std::vector<std::string> values;
    const std::size_t start = valueStart(json, findKey(json, key));
    if (start == std::string::npos || start >= json.size() || json[start] != '[') {
        return values;
    }

    for (std::size_t pos = start + 1; pos < json.size();) {
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] == ',') {
            ++pos;
            continue;
        }

        std::string value;
        if (!parseJsonStringAt(json, pos, &value)) break;
        values.push_back(value);

        ++pos;
        bool escaped = false;
        for (; pos < json.size(); ++pos) {
            const char c = json[pos];
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                ++pos;
                break;
            }
        }
    }
    return values;
}

}  // namespace

bool IntentJsonParser::parse(const std::string& text, IntentResult* result, std::string* error) const {
    if (!result) return false;
    IntentResult parsed;
    parsed.raw_json = extractJsonObject(text);
    if (parsed.raw_json.empty()) {
        if (error) *error = "no_json_object";
        *result = parsed;
        return false;
    }

    std::string intent_text;
    if (!getStringField(parsed.raw_json, "intent", &intent_text)) {
        if (error) *error = "missing_intent";
        *result = parsed;
        return false;
    }
    auto intent = intentTypeFromString(trim(intent_text));
    if (!intent) {
        if (error) *error = "invalid_intent";
        *result = parsed;
        return false;
    }
    parsed.intent = *intent;

    bool is_null = false;
    std::string device_json = getObjectField(parsed.raw_json, "device_command", &is_null);
    if (!device_json.empty()) {
        DeviceCommand command;
        getStringField(device_json, "room", &command.room);
        getStringField(device_json, "device", &command.device);
        getStringField(device_json, "action", &command.action);
        command.value = getOptionalNumberField(device_json, "value");
        parsed.device_command = command;
    }

    std::string event_json = getObjectField(parsed.raw_json, "device_event", &is_null);
    if (!event_json.empty()) {
        DeviceEvent event;
        getStringField(event_json, "room", &event.room);
        getStringField(event_json, "device", &event.device);
        getStringField(event_json, "event_type", &event.event_type);
        getStringField(event_json, "description", &event.description);
        parsed.device_event = event;
    }

    std::string memory_json = getObjectField(parsed.raw_json, "memory", &is_null);
    if (!memory_json.empty()) {
        MemoryItem item;
        getStringField(memory_json, "category", &item.category);
        getStringField(memory_json, "subject", &item.subject);
        getStringField(memory_json, "attribute", &item.attribute);
        getStringField(memory_json, "value", &item.value);
        parsed.memory = item;
    }

    std::string query_json = getObjectField(parsed.raw_json, "memory_query", &is_null);
    if (!query_json.empty()) {
        MemoryQuery query;
        getStringField(query_json, "subject", &query.subject);
        getStringField(query_json, "attribute", &query.attribute);
        parsed.memory_query = query;
    }

    std::string delete_json = getObjectField(parsed.raw_json, "memory_delete", &is_null);
    if (!delete_json.empty()) {
        MemoryDeleteRequest request;
        getStringField(delete_json, "category", &request.category);
        getStringField(delete_json, "subject", &request.subject);
        request.delete_all = getOptionalBoolField(delete_json, "delete_all").value_or(false);
        parsed.memory_delete = request;
    }

    std::string weather_json = getObjectField(parsed.raw_json, "weather_query", &is_null);
    if (!weather_json.empty()) {
        WeatherQuery query;
        getStringField(weather_json, "city", &query.city);
        getStringField(weather_json, "start_date", &query.start_date);
        getStringField(weather_json, "end_date", &query.end_date);
        query.days = static_cast<int>(getOptionalNumberField(weather_json, "days").value_or(0));
        parsed.weather_query = query;
    }

    parsed.missing_slots = getStringArrayField(parsed.raw_json, "missing_slots");
    getStringField(parsed.raw_json, "clarification_question", &parsed.clarification_question);
    getStringField(parsed.raw_json, "reply", &parsed.response_text);
    parsed.json_valid = true;
    *result = parsed;
    return true;
}

}  // namespace assistant
