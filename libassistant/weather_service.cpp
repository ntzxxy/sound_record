#include "weather_service.h"

#include <uapi/Client.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace assistant {
namespace {

constexpr int kMaxHistoryDays = 30;
constexpr std::size_t kMaxResponseBytes = 64 * 1024;

std::string trim(std::string value) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool hasControlCharacter(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

bool isLeapYear(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool parseDate(const std::string& text, int* year, int* month, int* day) {
    if (!year || !month || !day || text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    int parsed_year = 0;
    int parsed_month = 0;
    int parsed_day = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2d", &parsed_year, &parsed_month, &parsed_day) != 3 ||
        parsed_year < 1900 || parsed_year > 2100 || parsed_month < 1 || parsed_month > 12) {
        return false;
    }
    static constexpr std::array<int, 12> kDaysPerMonth =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = kDaysPerMonth[static_cast<std::size_t>(parsed_month - 1)];
    if (parsed_month == 2 && isLeapYear(parsed_year)) max_day = 29;
    if (parsed_day < 1 || parsed_day > max_day) return false;
    *year = parsed_year;
    *month = parsed_month;
    *day = parsed_day;
    return true;
}

// 返回公历日序号，不经过本地时区/DST，适合验证纯日期范围。
int daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

bool isJsonObject(const std::string& value) {
    const std::string trimmed = trim(value);
    return trimmed.size() >= 2 && trimmed.front() == '{' && trimmed.back() == '}';
}

}  // namespace

std::string WeatherQueryValidator::localToday() {
    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif
    char output[11] = {};
    std::strftime(output, sizeof(output), "%Y-%m-%d", &local_time);
    return output;
}

bool WeatherQueryValidator::normalize(const WeatherQuery& request,
                                      NormalizedWeatherQuery* normalized,
                                      std::string* error) {
    if (!normalized) return false;
    NormalizedWeatherQuery result;
    result.city = trim(request.city);
    if (result.city.empty()) {
        if (error) *error = "missing_city";
        return false;
    }
    if (result.city.size() > 80 || hasControlCharacter(result.city)) {
        if (error) *error = "invalid_city";
        return false;
    }

    result.start_date = trim(request.start_date);
    result.end_date = trim(request.end_date);
    if (result.start_date.empty() && result.end_date.empty()) {
        result.start_date = localToday();
        result.end_date = result.start_date;
    } else if (result.start_date.empty()) {
        result.start_date = result.end_date;
    } else if (result.end_date.empty()) {
        result.end_date = result.start_date;
    }

    int start_year = 0;
    int start_month = 0;
    int start_day = 0;
    int end_year = 0;
    int end_month = 0;
    int end_day = 0;
    if (!parseDate(result.start_date, &start_year, &start_month, &start_day) ||
        !parseDate(result.end_date, &end_year, &end_month, &end_day)) {
        if (error) *error = "invalid_date";
        return false;
    }

    const int start_serial = daysFromCivil(start_year, static_cast<unsigned>(start_month),
                                           static_cast<unsigned>(start_day));
    const int end_serial = daysFromCivil(end_year, static_cast<unsigned>(end_month),
                                         static_cast<unsigned>(end_day));
    const std::string today = localToday();
    const int today_year = std::stoi(today.substr(0, 4));
    const int today_month = std::stoi(today.substr(5, 2));
    const int today_day = std::stoi(today.substr(8, 2));
    const int today_serial = daysFromCivil(today_year, static_cast<unsigned>(today_month),
                                           static_cast<unsigned>(today_day));
    if (end_serial < start_serial) {
        if (error) *error = "invalid_date_range";
        return false;
    }
    if (end_serial > today_serial) {
        if (error) *error = "future_date_unsupported";
        return false;
    }

    const int derived_days = end_serial - start_serial + 1;
    if (derived_days > kMaxHistoryDays) {
        if (error) *error = "date_range_too_long";
        return false;
    }
    if (request.days < 0 || request.days > kMaxHistoryDays ||
        (request.days > 0 && request.days != derived_days)) {
        if (error) *error = "invalid_days";
        return false;
    }
    result.days = derived_days;
    *normalized = result;
    return true;
}

bool WeatherService::queryHistory(const NormalizedWeatherQuery& request, std::string* raw_json,
                                  std::string* runtime_context, std::string* error) const {
    if (!raw_json || !runtime_context) return false;
    try {
        const char* api_key = std::getenv("UAPI_API_KEY");
        uapi::Client client("https://uapis.cn", api_key ? api_key : "");
        const std::map<std::string, std::string> args = {
            {"city", request.city},
            {"start_date", request.start_date},
            {"end_date", request.end_date},
            {"days", std::to_string(request.days)},
            {"lang", "zh"},
        };
        const std::string response = client.misc().getMiscWeatherHistory(args);
        if (response.size() > kMaxResponseBytes || !isJsonObject(response)) {
            if (error) *error = "invalid_api_response";
            return false;
        }
        *raw_json = response;
        std::ostringstream context;
        context << "【可信 UAPI 天气查询结果】\n"
                << "查询地点：" << request.city << "；开始日期：" << request.start_date
                << "；结束日期：" << request.end_date << "；查询时长：" << request.days << " 天。\n"
                << "以下为接口返回的原始 JSON，只能依据其中字段回答，不能补造数据：\n"
                << response << "\n";
        *runtime_context = context.str();
        return true;
    } catch (const uapi::NotFoundError&) {
        if (error) *error = "location_not_found";
    } catch (const uapi::UnauthorizedError&) {
        if (error) *error = "unauthorized";
    } catch (const uapi::ServiceBusyError&) {
        if (error) *error = "service_busy";
    } catch (const uapi::UapiError&) {
        if (error) *error = "uapi_error";
    } catch (const std::exception&) {
        if (error) *error = "network_error";
    }
    return false;
}

}  // namespace assistant
