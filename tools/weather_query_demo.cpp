#include "weather_service.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: weather_query_demo <city> [start-date] [end-date]" << std::endl;
        return 1;
    }

    assistant::WeatherQuery request;
    request.city = argv[1];
    if (argc >= 3) request.start_date = argv[2];
    if (argc >= 4) request.end_date = argv[3];

    assistant::NormalizedWeatherQuery normalized;
    std::string error;
    if (!assistant::WeatherQueryValidator::normalize(request, &normalized, &error)) {
        std::cerr << "Weather query is invalid: " << error << std::endl;
        return 2;
    }

    assistant::WeatherService service;
    std::string json;
    std::string context;
    if (!service.queryHistory(normalized, &json, &context, &error)) {
        std::cerr << "Weather query failed: " << error << std::endl;
        return 3;
    }

    std::cout << json << std::endl;
    return 0;
}
