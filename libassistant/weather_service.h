#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

#include "assistant_types.h"

#include <string>

namespace assistant {

struct NormalizedWeatherQuery {
    std::string city;
    std::string start_date;
    std::string end_date;
    int days{0};
};

// 日期归一化与业务边界校验和联网逻辑分离，便于在不依赖网络的测试中覆盖。
class WeatherQueryValidator {
public:
    static bool normalize(const WeatherQuery& request, NormalizedWeatherQuery* normalized,
                          std::string* error);
    static std::string localToday();
};

class WeatherService {
public:
    bool queryHistory(const NormalizedWeatherQuery& request, std::string* raw_json,
                      std::string* runtime_context, std::string* error) const;
};

}  // namespace assistant

#endif  // WEATHER_SERVICE_H
