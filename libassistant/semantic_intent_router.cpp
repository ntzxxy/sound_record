#include "semantic_intent_router.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef SOUND_RECORD_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace assistant {
namespace {

constexpr std::size_t kEmbeddingSize = 512;
constexpr std::size_t kRouteCount = 7;

class RejectingSemanticIntentRouter final : public SemanticIntentRouter {
public:
    SemanticRouteResult classify(const std::string&) const override { return {}; }
};

#ifdef SOUND_RECORD_HAS_ONNXRUNTIME

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::istringstream stream(value);
    std::string part;
    while (std::getline(stream, part, delimiter)) parts.push_back(part);
    return parts;
}

std::string base64Decode(const std::string& input) {
    static constexpr unsigned char kInvalid = 0xFF;
    static const std::array<unsigned char, 256> table = [] {
        std::array<unsigned char, 256> out{};
        out.fill(kInvalid);
        const std::string alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t i = 0; i < alphabet.size(); ++i) {
            out[static_cast<unsigned char>(alphabet[i])] = static_cast<unsigned char>(i);
        }
        return out;
    }();
    std::string output;
    unsigned int buffer = 0;
    int bits = 0;
    for (unsigned char c : input) {
        if (c == '=') break;
        const unsigned char value = table[c];
        if (value == kInvalid) continue;
        buffer = (buffer << 6U) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((buffer >> bits) & 0xFFU));
        }
    }
    return output;
}

std::vector<std::string> utf8Characters(const std::string& text) {
    std::vector<std::string> characters;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if ((lead & 0xE0U) == 0xC0U) length = 2;
        else if ((lead & 0xF0U) == 0xE0U) length = 3;
        else if ((lead & 0xF8U) == 0xF0U) length = 4;
        if (i + length > text.size()) length = 1;
        characters.push_back(text.substr(i, length));
        i += length;
    }
    return characters;
}

bool isAsciiWhitespace(const std::string& character) {
    return character.size() == 1 &&
           (character[0] == ' ' || character[0] == '\t' || character[0] == '\n' ||
            character[0] == '\r' || character[0] == '\f');
}

bool isAsciiPunctuation(const std::string& character) {
    if (character.size() != 1) return false;
    const unsigned char c = static_cast<unsigned char>(character[0]);
    return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) ||
           (c >= 91 && c <= 96) || (c >= 123 && c <= 126);
}

bool isCjkOrWidePunctuation(const std::string& character) {
    if (character.size() < 3) return false;
    const unsigned char a = static_cast<unsigned char>(character[0]);
    const unsigned char b = static_cast<unsigned char>(character[1]);
    return (a >= 0xE4U && a <= 0xE9U) || (a == 0xE3U && b == 0x80U) ||
           (a == 0xEFU && b >= 0xBCU);
}

SemanticRoute routeFromString(const std::string& value) {
    if (value == "DEVICE_CONTROL") return SemanticRoute::DeviceControl;
    if (value == "MEMORY_WRITE") return SemanticRoute::MemoryWrite;
    if (value == "MEMORY_QUERY") return SemanticRoute::MemoryQuery;
    if (value == "MEMORY_DELETE") return SemanticRoute::MemoryDelete;
    if (value == "DEVICE_FAULT") return SemanticRoute::DeviceFault;
    if (value == "RECORD_QUERY") return SemanticRoute::RecordQuery;
    if (value == "WEATHER_QUERY") return SemanticRoute::WeatherQuery;
    return SemanticRoute::None;
}

std::size_t routeIndex(SemanticRoute route) {
    switch (route) {
        case SemanticRoute::DeviceControl: return 0;
        case SemanticRoute::MemoryWrite: return 1;
        case SemanticRoute::MemoryQuery: return 2;
        case SemanticRoute::MemoryDelete: return 3;
        case SemanticRoute::DeviceFault: return 4;
        case SemanticRoute::RecordQuery: return 5;
        case SemanticRoute::WeatherQuery: return 6;
        case SemanticRoute::None: return kRouteCount;
    }
    return kRouteCount;
}

SemanticRoute indexRoute(std::size_t index) {
    static constexpr SemanticRoute kRoutes[] = {
        SemanticRoute::DeviceControl, SemanticRoute::MemoryWrite,
        SemanticRoute::MemoryQuery, SemanticRoute::MemoryDelete,
        SemanticRoute::DeviceFault, SemanticRoute::RecordQuery,
        SemanticRoute::WeatherQuery,
    };
    return index < kRouteCount ? kRoutes[index] : SemanticRoute::None;
}

struct Prototype {
    SemanticRoute route{SemanticRoute::None};
    std::string text;
    std::array<float, kEmbeddingSize> dense{};
    std::vector<std::pair<int, float>> sparse;
};

class OnnxHybridSemanticIntentRouter final : public SemanticIntentRouter {
public:
    explicit OnnxHybridSemanticIntentRouter(std::string model_directory)
        : model_directory_(std::move(model_directory)),
          env_(ORT_LOGGING_LEVEL_WARNING, "sound_record_semantic_router") {
        loadConfig();
        loadVocabulary();
        loadTfidf();
        loadPrototypes();
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetIntraOpNumThreads(1);
        session_ = std::make_unique<Ort::Session>(
            env_, (model_directory_ + "/model.onnx").c_str(), options);
        if (session_->GetInputCount() < 2 || session_->GetOutputCount() < 1) {
            throw std::runtime_error("unexpected ONNX input/output count");
        }
    }

    SemanticRouteResult classify(const std::string& text) const override {
        const auto started = std::chrono::steady_clock::now();
        SemanticRouteResult result;
        try {
            const auto dense = embed(text);
            const auto sparse = sparseVector(text);
            std::array<float, kRouteCount> dense_scores;
            std::array<float, kRouteCount> sparse_scores;
            std::array<float, kRouteCount> route_scores;
            dense_scores.fill(-1.0F);
            sparse_scores.fill(-1.0F);
            std::array<std::size_t, kRouteCount> matches{};
            for (std::size_t i = 0; i < prototypes_.size(); ++i) {
                const Prototype& prototype = prototypes_[i];
                const std::size_t route_index = routeIndex(prototype.route);
                if (route_index >= kRouteCount) continue;
                float dense_score = 0.0F;
                for (std::size_t j = 0; j < kEmbeddingSize; ++j) {
                    dense_score += dense[j] * prototype.dense[j];
                }
                float sparse_score = 0.0F;
                for (const auto& [index, value] : prototype.sparse) {
                    const auto it = sparse.find(index);
                    if (it != sparse.end()) sparse_score += value * it->second;
                }
                if (dense_score > dense_scores[route_index]) {
                    dense_scores[route_index] = dense_score;
                    matches[route_index] = i;
                }
                sparse_scores[route_index] = std::max(sparse_scores[route_index], sparse_score);
            }
            // Match the validated Python router: each representation chooses
            // its best route prototype before the frozen weighted fusion.
            for (std::size_t i = 0; i < kRouteCount; ++i) {
                route_scores[i] = dense_weight_ * dense_scores[i] +
                                  sparse_weight_ * sparse_scores[i];
            }
            std::array<std::size_t, kRouteCount> order{};
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
                return route_scores[left] > route_scores[right];
            });
            result.top1_score = route_scores[order[0]];
            result.top2_score = route_scores[order[1]];
            result.margin = result.top1_score - result.top2_score;
            result.matched_example = prototypes_[matches[order[0]]].text;
            result.route = (result.top1_score >= score_threshold_ &&
                            result.margin >= margin_threshold_)
                               ? indexRoute(order[0])
                               : SemanticRoute::None;
            result.available = true;
        } catch (const std::exception& error) {
            std::cerr << "[SemanticIntentRouter] inference_failed=" << error.what() << std::endl;
        }
        const auto finished = std::chrono::steady_clock::now();
        result.latency_ms = std::chrono::duration<double, std::milli>(finished - started).count();
        return result;
    }

private:
    void loadConfig() {
        std::ifstream input(model_directory_ + "/router_config.tsv");
        if (!input) throw std::runtime_error("router_config.tsv is missing");
        std::string line;
        while (std::getline(input, line)) {
            const auto parts = split(line, '\t');
            if (parts.size() != 2) continue;
            if (parts[0] == "dense_weight") dense_weight_ = std::stof(parts[1]);
            else if (parts[0] == "sparse_weight") sparse_weight_ = std::stof(parts[1]);
            else if (parts[0] == "score_threshold") score_threshold_ = std::stof(parts[1]);
            else if (parts[0] == "margin_threshold") margin_threshold_ = std::stof(parts[1]);
            else if (parts[0] == "max_length") max_length_ = static_cast<std::size_t>(std::stoul(parts[1]));
        }
    }

    void loadVocabulary() {
        std::ifstream input(model_directory_ + "/vocab.txt");
        if (!input) throw std::runtime_error("vocab.txt is missing");
        std::string token;
        int64_t id = 0;
        while (std::getline(input, token)) vocab_[token] = id++;
        cls_id_ = vocab_.at("[CLS]");
        sep_id_ = vocab_.at("[SEP]");
        pad_id_ = vocab_.at("[PAD]");
        unknown_id_ = vocab_.at("[UNK]");
    }

    void loadTfidf() {
        std::ifstream input(model_directory_ + "/tfidf.tsv");
        if (!input) throw std::runtime_error("tfidf.tsv is missing");
        std::string line;
        while (std::getline(input, line)) {
            const auto parts = split(line, '\t');
            if (parts.size() != 3) continue;
            tfidf_[base64Decode(parts[2])] = {std::stoi(parts[0]), std::stof(parts[1])};
        }
    }

    void loadPrototypes() {
        std::ifstream input(model_directory_ + "/prototypes.tsv");
        if (!input) throw std::runtime_error("prototypes.tsv is missing");
        std::string line;
        while (std::getline(input, line)) {
            const auto parts = split(line, '\t');
            if (parts.size() != 4) continue;
            Prototype prototype;
            prototype.route = routeFromString(parts[0]);
            prototype.text = base64Decode(parts[1]);
            const auto dense_values = split(parts[2], ',');
            if (dense_values.size() != kEmbeddingSize) {
                throw std::runtime_error("invalid prototype embedding width");
            }
            for (std::size_t i = 0; i < dense_values.size(); ++i) {
                prototype.dense[i] = std::stof(dense_values[i]);
            }
            for (const std::string& item : split(parts[3], ',')) {
                const auto pair = split(item, ':');
                if (pair.size() == 2) prototype.sparse.emplace_back(std::stoi(pair[0]), std::stof(pair[1]));
            }
            prototypes_.push_back(std::move(prototype));
        }
        if (prototypes_.empty()) throw std::runtime_error("no semantic prototypes loaded");
    }

    std::vector<std::string> basicTokens(const std::string& text) const {
        std::vector<std::string> tokens;
        std::string current;
        auto flush = [&] {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        };
        for (std::string character : utf8Characters(text)) {
            if (character.size() == 1 && character[0] >= 'A' && character[0] <= 'Z') {
                character[0] = static_cast<char>(character[0] - 'A' + 'a');
            }
            if (isAsciiWhitespace(character)) {
                flush();
            } else if (isAsciiPunctuation(character) || isCjkOrWidePunctuation(character)) {
                flush();
                tokens.push_back(character);
            } else if (character.size() > 1) {
                flush();
                tokens.push_back(character);
            } else {
                current += character;
            }
        }
        flush();
        return tokens;
    }

    std::vector<int64_t> tokenIds(const std::string& text) const {
        std::vector<int64_t> ids{cls_id_};
        for (const std::string& token : basicTokens(text)) {
            const auto exact = vocab_.find(token);
            if (exact != vocab_.end()) {
                ids.push_back(exact->second);
                continue;
            }
            const auto characters = utf8Characters(token);
            std::size_t start = 0;
            bool bad = false;
            std::vector<int64_t> pieces;
            while (start < characters.size()) {
                std::size_t end = characters.size();
                bool found = false;
                while (end > start) {
                    std::string piece;
                    for (std::size_t i = start; i < end; ++i) piece += characters[i];
                    if (start > 0) piece = "##" + piece;
                    const auto it = vocab_.find(piece);
                    if (it != vocab_.end()) {
                        pieces.push_back(it->second);
                        start = end;
                        found = true;
                        break;
                    }
                    --end;
                }
                if (!found) {
                    bad = true;
                    break;
                }
            }
            if (bad) ids.push_back(unknown_id_);
            else ids.insert(ids.end(), pieces.begin(), pieces.end());
            if (ids.size() + 1 >= max_length_) break;
        }
        ids.push_back(sep_id_);
        if (ids.size() > max_length_) ids.resize(max_length_);
        return ids;
    }

    std::array<float, kEmbeddingSize> embed(const std::string& text) const {
        std::vector<int64_t> ids = tokenIds(text);
        const std::size_t token_count = ids.size();
        std::vector<int64_t> attention(max_length_, 0);
        std::vector<int64_t> type_ids(max_length_, 0);
        ids.resize(max_length_, pad_id_);
        std::fill(attention.begin(), attention.begin() + static_cast<std::ptrdiff_t>(token_count), 1);
        const std::array<int64_t, 2> shape{1, static_cast<int64_t>(max_length_)};
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<Ort::Value, 3> inputs{
            Ort::Value::CreateTensor<int64_t>(memory, ids.data(), ids.size(), shape.data(), shape.size()),
            Ort::Value::CreateTensor<int64_t>(memory, attention.data(), attention.size(), shape.data(), shape.size()),
            Ort::Value::CreateTensor<int64_t>(memory, type_ids.data(), type_ids.size(), shape.data(), shape.size()),
        };
        const std::array<const char*, 3> input_names{"input_ids", "attention_mask", "token_type_ids"};
        const std::array<const char*, 1> output_names{"last_hidden_state"};
        auto outputs = session_->Run(Ort::RunOptions{nullptr}, input_names.data(), inputs.data(),
                                     inputs.size(), output_names.data(), output_names.size());
        const float* values = outputs.front().GetTensorData<float>();
        std::array<float, kEmbeddingSize> embedding{};
        float norm = 0.0F;
        for (std::size_t i = 0; i < kEmbeddingSize; ++i) {
            embedding[i] = values[i];
            norm += values[i] * values[i];
        }
        norm = std::sqrt(std::max(norm, 1e-12F));
        for (float& value : embedding) value /= norm;
        return embedding;
    }

    std::unordered_map<int, float> sparseVector(const std::string& text) const {
        std::string normalized;
        bool last_space = false;
        for (std::string character : utf8Characters(text)) {
            if (character.size() == 1 && character[0] >= 'A' && character[0] <= 'Z') {
                character[0] = static_cast<char>(character[0] - 'A' + 'a');
            }
            if (isAsciiWhitespace(character)) {
                if (!last_space) normalized.push_back(' ');
                last_space = true;
            } else {
                normalized += character;
                last_space = false;
            }
        }
        const auto chars = utf8Characters(normalized);
        std::unordered_map<std::string, int> counts;
        for (std::size_t n = 2; n <= 4; ++n) {
            if (chars.size() < n) continue;
            for (std::size_t i = 0; i + n <= chars.size(); ++i) {
                std::string term;
                for (std::size_t j = i; j < i + n; ++j) term += chars[j];
                if (tfidf_.find(term) != tfidf_.end()) ++counts[term];
            }
        }
        std::unordered_map<int, float> values;
        float norm = 0.0F;
        for (const auto& [term, count] : counts) {
            const auto [index, idf] = tfidf_.at(term);
            const float value = (1.0F + std::log(static_cast<float>(count))) * idf;
            values[index] = value;
            norm += value * value;
        }
        norm = std::sqrt(std::max(norm, 1e-12F));
        for (auto& entry : values) entry.second /= norm;
        return values;
    }

    std::string model_directory_;
    float dense_weight_{0.8F};
    float sparse_weight_{0.2F};
    float score_threshold_{0.53F};
    float margin_threshold_{0.01F};
    std::size_t max_length_{128};
    std::unordered_map<std::string, int64_t> vocab_;
    std::unordered_map<std::string, std::pair<int, float>> tfidf_;
    std::vector<Prototype> prototypes_;
    int64_t cls_id_{101};
    int64_t sep_id_{102};
    int64_t pad_id_{0};
    int64_t unknown_id_{100};
    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
};

#endif  // SOUND_RECORD_HAS_ONNXRUNTIME

}  // namespace

std::shared_ptr<const SemanticIntentRouter> createSemanticIntentRouterFromEnvironment() {
    const char* model_directory = std::getenv("SOUND_RECORD_SEMANTIC_ROUTER_MODEL_DIR");
    if (!model_directory || !*model_directory) {
        return std::make_shared<RejectingSemanticIntentRouter>();
    }
#ifdef SOUND_RECORD_HAS_ONNXRUNTIME
    try {
        return std::make_shared<OnnxHybridSemanticIntentRouter>(model_directory);
    } catch (const std::exception& error) {
        std::cerr << "[SemanticIntentRouter] initialization_failed=" << error.what() << std::endl;
    }
#else
    std::cerr << "[SemanticIntentRouter] ONNX Runtime support was not built" << std::endl;
#endif
    return std::make_shared<RejectingSemanticIntentRouter>();
}

}  // namespace assistant
