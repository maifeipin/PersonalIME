#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "../types.h"

namespace ime {

class SmartMacroProcessor {
public:
    static bool IsMacroQuery(const std::string& rawCode) {
        std::string lower = rawCode;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

        // 'date' is supported for date expansion. 'rq' is intentionally excluded to avoid collision with high-frequency Wubi word '换'
        if (lower == "date" || lower == "time" || lower == "sj" || lower == "week" || lower == "xq" || lower == "now") {
            return true;
        }

        // Financial uppercase numbers: v followed by digits and at most one decimal point (e.g. v12345 or v100.5)
        if (lower.length() >= 2 && lower.front() == 'v') {
            bool hasDigit = false;
            int dotCount = 0;
            for (size_t i = 1; i < lower.length(); ++i) {
                char c = lower[i];
                if (c >= '0' && c <= '9') {
                    hasDigit = true;
                } else if (c == '.') {
                    dotCount++;
                    if (dotCount > 1) return false;
                } else {
                    return false;
                }
            }
            return hasDigit;
        }

        return false;
    }

    static std::vector<Candidate> ExpandMacro(const std::string& rawCode, std::chrono::system_clock::time_point nowPoint = std::chrono::system_clock::now()) {
        std::vector<Candidate> result;
        std::string lower = rawCode;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

        std::time_t t = std::chrono::system_clock::to_time_t(nowPoint);
        std::tm tmNow{};
#ifdef _WIN32
        localtime_s(&tmNow, &t);
#else
        localtime_r(&t, &tmNow);
#endif

        int year = tmNow.tm_year + 1900;
        int month = tmNow.tm_mon + 1;
        int day = tmNow.tm_mday;
        int hour = tmNow.tm_hour;
        int min = tmNow.tm_min;
        int sec = tmNow.tm_sec;
        int wday = tmNow.tm_wday; // 0 = Sunday, 1 = Monday...

        auto makeCandidate = [&](const std::wstring& text, const std::string& idSuffix) -> Candidate {
            Candidate c;
            c.entry_id = "macro_" + lower + "_" + idSuffix;
            c.text = text;
            c.raw_code = rawCode;
            c.matched_code = rawCode;
            c.resolved_code = rawCode;
            c.dict_id = DICT_ID_CUSTOM;
            c.scheme = InputScheme::Custom;
            c.quality = MatchQuality::ExactFull;
            c.dict_priority = -1; // Highest priority
            c.method_priority = -1;
            c.score = 100000.0; // Top rank
            c.base_weight = 100000.0;
            return c;
        };

        if (lower == "date") {
            // 1. 2026年11月7日
            std::wstringstream ss1;
            ss1 << year << L"年" << month << L"月" << day << L"日";
            result.push_back(makeCandidate(ss1.str(), "1"));

            // 2. 2026-11-07
            std::wstringstream ss2;
            ss2 << year << L"-" << std::setfill(L'0') << std::setw(2) << month << L"-" << std::setw(2) << day;
            result.push_back(makeCandidate(ss2.str(), "2"));

            // 3. 2026/11/07
            std::wstringstream ss3;
            ss3 << year << L"/" << std::setfill(L'0') << std::setw(2) << month << L"/" << std::setw(2) << day;
            result.push_back(makeCandidate(ss3.str(), "3"));

            // 4. 20261107
            std::wstringstream ss4;
            ss4 << year << std::setfill(L'0') << std::setw(2) << month << std::setw(2) << day;
            result.push_back(makeCandidate(ss4.str(), "4"));
        }
        else if (lower == "time" || lower == "sj") {
            // 1. 14:45
            std::wstringstream ss1;
            ss1 << std::setfill(L'0') << std::setw(2) << hour << L":" << std::setw(2) << min;
            result.push_back(makeCandidate(ss1.str(), "1"));

            // 2. 14:45:20
            std::wstringstream ss2;
            ss2 << std::setfill(L'0') << std::setw(2) << hour << L":" << std::setw(2) << min << L":" << std::setw(2) << sec;
            result.push_back(makeCandidate(ss2.str(), "2"));

            // 3. 14时45分20秒
            std::wstringstream ss3;
            ss3 << hour << L"时" << min << L"分" << sec << L"秒";
            result.push_back(makeCandidate(ss3.str(), "3"));
        }
        else if (lower == "week" || lower == "xq") {
            static const wchar_t* const wdays[] = { L"星期日", L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六" };
            static const wchar_t* const zhours[] = { L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六" };
            int idx = (wday >= 0 && wday <= 6) ? wday : 0;
            result.push_back(makeCandidate(wdays[idx], "1"));
            result.push_back(makeCandidate(zhours[idx], "2"));
        }
        else if (lower == "now") {
            std::wstringstream ss1;
            ss1 << year << L"-" << std::setfill(L'0') << std::setw(2) << month << L"-" << std::setw(2) << day
                << L" " << std::setw(2) << hour << L":" << std::setw(2) << min << L":" << std::setw(2) << sec;
            result.push_back(makeCandidate(ss1.str(), "1"));

            std::wstringstream ss2;
            ss2 << year << L"年" << month << L"月" << day << L"日 " << hour << L"时" << min << L"分";
            result.push_back(makeCandidate(ss2.str(), "2"));
        }
        else if (lower.length() >= 2 && lower.front() == 'v') {
            std::string numStr = lower.substr(1);
            auto cands = ConvertFinancialNumber(numStr);
            for (size_t i = 0; i < cands.size(); ++i) {
                result.push_back(makeCandidate(cands[i], std::to_string(i + 1)));
            }
        }

        return result;
    }

private:
    static std::vector<std::wstring> ConvertFinancialNumber(const std::string& str) {
        std::vector<std::wstring> list;
        if (str.empty()) return list;

        size_t dotPos = str.find('.');
        std::string intPart = (dotPos == std::string::npos) ? str : str.substr(0, dotPos);
        std::string fracPart = (dotPos == std::string::npos) ? "" : str.substr(dotPos + 1);

        if (intPart.empty() && fracPart.empty()) return list;
        if (intPart.length() > 12) return list; // Limit to hundreds of billions

        static const wchar_t* const digitsUpper = L"零壹贰叁肆伍陆柒捌玖";
        static const wchar_t* const digitsLower = L"零一二三四五六七八九";
        static const wchar_t* const unitsInt = L"拾佰仟";

        auto convertSection = [](long long n, bool upper) -> std::wstring {
            const wchar_t* d = upper ? digitsUpper : digitsLower;
            std::wstring res;
            int qian = n / 1000;
            int bai = (n % 1000) / 100;
            int shi = (n % 100) / 10;
            int ge = n % 10;

            bool zero = false;
            if (qian > 0) { res += d[qian]; res += (upper ? L'仟' : L'千'); }
            else if (!res.empty()) zero = true;

            if (bai > 0) { if (zero) { res += d[0]; zero = false; } res += d[bai]; res += (upper ? L'佰' : L'百'); }
            else if (!res.empty()) zero = true;

            if (shi > 0) {
                if (zero) { res += d[0]; zero = false; }
                if (!upper && shi == 1 && res.empty()) {
                    // "一十" -> "十"
                    res += L'十';
                } else {
                    res += d[shi]; res += (upper ? L'拾' : L'十');
                }
            } else if (!res.empty() && ge > 0) zero = true;

            if (ge > 0) { if (zero) { res += d[0]; zero = false; } res += d[ge]; }
            return res;
        };

        auto convertInt = [&](long long val, bool upper) -> std::wstring {
            if (val == 0) return std::wstring(1, upper ? digitsUpper[0] : digitsLower[0]);
            long long yi = val / 100000000LL;
            long long wan = (val % 100000000LL) / 10000LL;
            long long ge = val % 10000LL;

            std::wstring res;
            if (yi > 0) {
                res += convertSection(yi, upper) + (upper ? L'亿' : L'亿');
            }
            if (wan > 0) {
                if (!res.empty() && wan < 1000) res += (upper ? digitsUpper[0] : digitsLower[0]);
                res += convertSection(wan, upper) + (upper ? L'万' : L'万');
            }
            if (ge > 0) {
                if (!res.empty() && ge < 1000) res += (upper ? digitsUpper[0] : digitsLower[0]);
                res += convertSection(ge, upper);
            }
            return res;
        };

        try {
            long long intVal = intPart.empty() ? 0 : std::stoll(intPart);
            // 1. 金额大写 (壹万贰仟叁佰肆拾伍元陆角柒分) - 仅当小数位数不超过2位时提供，超过时不静默截断
            if (fracPart.length() <= 2) {
                std::wstring currency = convertInt(intVal, true) + L"元";
                if (fracPart.empty() || fracPart == "0" || fracPart == "00") {
                    currency += L"整";
                } else {
                    int jiao = fracPart[0] - '0';
                    int fen = (fracPart.length() > 1) ? (fracPart[1] - '0') : 0;
                    if (jiao > 0) { currency += digitsUpper[jiao]; currency += L"角"; }
                    else if (fen > 0) { currency += digitsUpper[0]; }
                    if (fen > 0) { currency += digitsUpper[fen]; currency += L"分"; }
                }
                list.push_back(currency);
            }

            // 2. 传统大写数字 (壹万贰仟叁佰肆拾伍点...)
            std::wstring pureUpper = convertInt(intVal, true);
            if (!fracPart.empty()) {
                pureUpper += L"点";
                for (char c : fracPart) {
                    if (c >= '0' && c <= '9') pureUpper += digitsUpper[c - '0'];
                }
            }
            list.push_back(pureUpper);

            // 3. 常见中文小写数字 (一万二千三百四十五点...)
            std::wstring pureLower = convertInt(intVal, false);
            if (!fracPart.empty()) {
                pureLower += L"点";
                for (char c : fracPart) {
                    if (c >= '0' && c <= '9') pureLower += digitsLower[c - '0'];
                }
            }
            list.push_back(pureLower);
        } catch (...) {}

        return list;
    }
};

} // namespace ime
