#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <direct.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Database.h"

using namespace std;

struct HttpRequest {
    string method;
    string path;
    string version;
    unordered_map<string, string> headers;
    string body;
};

static string trim(const string& s) {
    size_t start = 0;
    while (start < s.size() && isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }

    size_t end = s.size();
    while (end > start && isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

static string toLowerCopy(string s) {
    transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

static string htmlEscape(const string& s) {
    string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

static string jsonEscape(const string& s) {
    string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "?";
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

static string urlDecode(const string& input) {
    string output;
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '+') {
            output.push_back(' ');
            continue;
        }

        if (input[i] == '%' && i + 2 < input.size()) {
            char hex[3] = {input[i + 1], input[i + 2], '\0'};
            char* end = nullptr;
            long value = strtol(hex, &end, 16);
            if (end && *end == '\0') {
                output.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }

        output.push_back(input[i]);
    }

    return output;
}

static unordered_map<string, string> parseForm(const string& body) {
    unordered_map<string, string> values;
    size_t start = 0;

    while (start <= body.size()) {
        size_t amp = body.find('&', start);
        if (amp == string::npos) {
            amp = body.size();
        }

        string part = body.substr(start, amp - start);
        size_t eq = part.find('=');
        string key;
        string value;

        if (eq == string::npos) {
            key = urlDecode(part);
        } else {
            key = urlDecode(part.substr(0, eq));
            value = urlDecode(part.substr(eq + 1));
        }

        if (!key.empty()) {
            values[key] = value;
        }

        if (amp == body.size()) {
            break;
        }

        start = amp + 1;
    }

    return values;
}

struct MultipartFilePart {
    string fieldName;
    string fileName;
    string contentType;
    string data;
};

static string extractQuotedParam(const string& source, const string& key) {
    const string pattern = key + "=\"";
    size_t start = source.find(pattern);
    if (start == string::npos) {
        return "";
    }

    start += pattern.size();
    size_t end = source.find('"', start);
    if (end == string::npos) {
        return "";
    }
    return source.substr(start, end - start);
}

static bool extractMultipartBoundary(const string& contentType, string& boundary) {
    boundary.clear();
    if (contentType.empty()) {
        return false;
    }

    string lower = toLowerCopy(contentType);
    size_t typePos = lower.find("multipart/form-data");
    if (typePos == string::npos) {
        return false;
    }

    size_t boundaryPos = lower.find("boundary=");
    if (boundaryPos == string::npos) {
        return false;
    }

    size_t valueStart = boundaryPos + 9;
    boundary = trim(contentType.substr(valueStart));
    size_t semi = boundary.find(';');
    if (semi != string::npos) {
        boundary = trim(boundary.substr(0, semi));
    }

    if (!boundary.empty() && boundary.front() == '"' && boundary.back() == '"' && boundary.size() >= 2) {
        boundary = boundary.substr(1, boundary.size() - 2);
    }

    return !boundary.empty();
}

static bool parseMultipartFileField(const string& body,
                                    const string& boundary,
                                    const string& targetField,
                                    MultipartFilePart& outPart) {
    const string marker = "--" + boundary;
    size_t searchPos = 0;

    while (true) {
        size_t markerPos = body.find(marker, searchPos);
        if (markerPos == string::npos) {
            return false;
        }

        size_t partStart = markerPos + marker.size();
        if (partStart + 1 < body.size() && body.compare(partStart, 2, "--") == 0) {
            return false;
        }
        if (partStart + 1 < body.size() && body.compare(partStart, 2, "\r\n") == 0) {
            partStart += 2;
        }

        size_t headersEnd = body.find("\r\n\r\n", partStart);
        if (headersEnd == string::npos) {
            return false;
        }

        string headerBlock = body.substr(partStart, headersEnd - partStart);
        string disposition;
        string partContentType;

        istringstream hs(headerBlock);
        string line;
        while (getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            size_t colon = line.find(':');
            if (colon == string::npos) {
                continue;
            }

            string key = toLowerCopy(trim(line.substr(0, colon)));
            string value = trim(line.substr(colon + 1));
            if (key == "content-disposition") {
                disposition = value;
            } else if (key == "content-type") {
                partContentType = value;
            }
        }

        size_t dataStart = headersEnd + 4;
        size_t nextBoundaryPos = body.find("\r\n" + marker, dataStart);
        if (nextBoundaryPos == string::npos) {
            return false;
        }

        string fieldName = extractQuotedParam(disposition, "name");
        string fileName = extractQuotedParam(disposition, "filename");
        if (fieldName == targetField && !fileName.empty()) {
            outPart.fieldName = fieldName;
            outPart.fileName = fileName;
            outPart.contentType = partContentType;
            outPart.data = body.substr(dataStart, nextBoundaryPos - dataStart);
            return true;
        }

        searchPos = nextBoundaryPos + 2;
    }
}

static bool sendAll(SOCKET socketFd, const string& data) {
    size_t totalSent = 0;
    while (totalSent < data.size()) {
        int sent = send(socketFd, data.data() + totalSent, static_cast<int>(data.size() - totalSent), 0);
        if (sent <= 0) {
            return false;
        }
        totalSent += static_cast<size_t>(sent);
    }
    return true;
}

static void sendResponse(SOCKET client,
                         int statusCode,
                         const string& statusText,
                         const string& contentType,
                         const string& body,
                         const vector<pair<string, string>>& extraHeaders = {}) {
    ostringstream response;
    response << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n";

    for (const auto& h : extraHeaders) {
        response << h.first << ": " << h.second << "\r\n";
    }

    response << "\r\n";
    response << body;
    sendAll(client, response.str());
}

static void sendRedirect(SOCKET client,
                         const string& location,
                         const vector<pair<string, string>>& extraHeaders = {}) {
    vector<pair<string, string>> headers = extraHeaders;
    headers.push_back({"Location", location});
    sendResponse(client, 302, "Found", "text/plain; charset=utf-8", "Redirecting...", headers);
}

static bool readHttpRequest(SOCKET client, string& raw) {
    raw.clear();
    char buffer[4096];
    size_t headerEnd = string::npos;
    size_t contentLength = 0;

    while (true) {
        int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            return false;
        }

        raw.append(buffer, static_cast<size_t>(received));

        if (headerEnd == string::npos) {
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != string::npos) {
                string headerPart = raw.substr(0, headerEnd);
                istringstream hs(headerPart);
                string line;
                getline(hs, line);

                while (getline(hs, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }

                    size_t colon = line.find(':');
                    if (colon == string::npos) {
                        continue;
                    }

                    string key = toLowerCopy(trim(line.substr(0, colon)));
                    string value = trim(line.substr(colon + 1));
                    if (key == "content-length") {
                        try {
                            contentLength = static_cast<size_t>(stoul(value));
                        } catch (...) {
                            contentLength = 0;
                        }
                    }
                }
            }
        }

        if (headerEnd != string::npos) {
            size_t needed = headerEnd + 4 + contentLength;
            if (raw.size() >= needed) {
                return true;
            }
        }
    }
}

static HttpRequest parseHttpRequest(const string& raw) {
    HttpRequest request;
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == string::npos) {
        return request;
    }

    string headerPart = raw.substr(0, headerEnd);
    request.body = raw.substr(headerEnd + 4);

    istringstream hs(headerPart);
    string line;
    if (!getline(hs, line)) {
        return request;
    }

    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    {
        istringstream rl(line);
        rl >> request.method >> request.path >> request.version;
    }

    while (getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t colon = line.find(':');
        if (colon == string::npos) {
            continue;
        }

        string key = toLowerCopy(trim(line.substr(0, colon)));
        string value = trim(line.substr(colon + 1));
        request.headers[key] = value;
    }

    size_t qPos = request.path.find('?');
    if (qPos != string::npos) {
        request.path = request.path.substr(0, qPos);
    }

    return request;
}

static string getMimeType(const string& filePath) {
    size_t dot = filePath.rfind('.');
    string ext = dot == string::npos ? "" : toLowerCopy(filePath.substr(dot));

    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}

static bool readFile(const string& filePath, string& data) {
    ifstream file(filePath, ios::binary);
    if (!file) {
        return false;
    }

    ostringstream buffer;
    buffer << file.rdbuf();
    data = buffer.str();
    return true;
}

static bool writeFile(const string& filePath, const string& data) {
    ofstream file(filePath, ios::binary | ios::trunc);
    if (!file) {
        return false;
    }
    file.write(data.data(), static_cast<streamsize>(data.size()));
    return file.good();
}

static bool contentTypeToPhotoFormat(const string& rawContentType, string& ext, string& mime) {
    const string contentType = toLowerCopy(trim(rawContentType));
    if (contentType == "image/png") {
        ext = ".png";
        mime = "image/png";
        return true;
    }
    if (contentType == "image/jpeg" || contentType == "image/jpg") {
        ext = ".jpg";
        mime = "image/jpeg";
        return true;
    }
    if (contentType == "image/webp") {
        ext = ".webp";
        mime = "image/webp";
        return true;
    }
    return false;
}

static bool detectPhotoFormatBySignature(const string& data, string& ext, string& mime) {
    if (data.size() >= 8) {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(data.data());
        if (b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47 &&
            b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A) {
            ext = ".png";
            mime = "image/png";
            return true;
        }
    }

    if (data.size() >= 3) {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(data.data());
        if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
            ext = ".jpg";
            mime = "image/jpeg";
            return true;
        }
    }

    if (data.size() >= 12) {
        if (data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0) {
            ext = ".webp";
            mime = "image/webp";
            return true;
        }
    }

    return false;
}

static bool inferPhotoFormat(const MultipartFilePart& part, string& ext, string& mime) {
    ext.clear();
    mime.clear();
    if (detectPhotoFormatBySignature(part.data, ext, mime)) {
        return true;
    }
    return contentTypeToPhotoFormat(part.contentType, ext, mime);
}

static bool isSafeStoredProfilePhotoPath(const string& path) {
    if (path.empty()) {
        return false;
    }
    if (path.find("..") != string::npos || path.find(':') != string::npos || path.find('\\') != string::npos) {
        return false;
    }
    return path.rfind("uploads/profile/", 0) == 0;
}

static bool ensureProfileUploadsDirectory() {
    if (_mkdir("uploads") != 0 && errno != EEXIST) {
        return false;
    }
    if (_mkdir("uploads\\profile") != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static string generateSessionToken() {
    static random_device rd;
    static mt19937_64 gen(rd());
    uniform_int_distribution<unsigned long long> dist;

    unsigned long long a = dist(gen);
    unsigned long long b = dist(gen);
    unsigned long long c = static_cast<unsigned long long>(
        chrono::high_resolution_clock::now().time_since_epoch().count());

    ostringstream os;
    os << hex << a << b << c;
    return os.str();
}

static string getCookieValue(const string& cookieHeader, const string& key) {
    size_t start = 0;
    while (start < cookieHeader.size()) {
        size_t semi = cookieHeader.find(';', start);
        if (semi == string::npos) {
            semi = cookieHeader.size();
        }

        string item = trim(cookieHeader.substr(start, semi - start));
        size_t eq = item.find('=');
        if (eq != string::npos) {
            string k = trim(item.substr(0, eq));
            string v = trim(item.substr(eq + 1));
            if (k == key) {
                return v;
            }
        }

        if (semi == cookieHeader.size()) {
            break;
        }
        start = semi + 1;
    }
    return "";
}

static long long currentEpochSeconds() {
    return static_cast<long long>(chrono::duration_cast<chrono::seconds>(
        chrono::system_clock::now().time_since_epoch()).count());
}

static string formatDateTime(long long epochSeconds) {
    if (epochSeconds <= 0) {
        return "Not scheduled";
    }

    time_t rawTime = static_cast<time_t>(epochSeconds);
    tm* localTimePtr = localtime(&rawTime);
    if (!localTimePtr) {
        return "Invalid time";
    }
    tm localTime = *localTimePtr;

    char buffer[32];
    if (strftime(buffer, sizeof(buffer), "%Y-%m-%d %I:%M %p", &localTime) == 0) {
        return "Invalid time";
    }

    return buffer;
}

static bool isSupportedRepeatUnit(const string& unit) {
    return unit == "minute" ||
           unit == "hour" ||
           unit == "day" ||
           unit == "week" ||
           unit == "month" ||
           unit == "year";
}

static bool isSupportedRepeatMode(const string& repeatMode) {
    return repeatMode == "every_day" ||
           repeatMode == "selected_days" ||
           repeatMode == "every_x_days";
}

static bool isSupportedDurationType(const string& durationType) {
    return durationType == "continuous" || durationType == "number_of_days";
}

static bool parseTime24(const string& value, int& hour, int& minute) {
    if (value.size() != 5 || value[2] != ':') {
        return false;
    }

    if (!isdigit(static_cast<unsigned char>(value[0])) ||
        !isdigit(static_cast<unsigned char>(value[1])) ||
        !isdigit(static_cast<unsigned char>(value[3])) ||
        !isdigit(static_cast<unsigned char>(value[4]))) {
        return false;
    }

    try {
        hour = stoi(value.substr(0, 2));
        minute = stoi(value.substr(3, 2));
    } catch (...) {
        return false;
    }

    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

static bool parseDateYmd(const string& value, int& year, int& month, int& day) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
        return false;
    }

    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (!isdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }

    try {
        year = stoi(value.substr(0, 4));
        month = stoi(value.substr(5, 2));
        day = stoi(value.substr(8, 2));
    } catch (...) {
        return false;
    }

    if (year < 1970 || year > 3000 || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }

    tm testDate{};
    testDate.tm_year = year - 1900;
    testDate.tm_mon = month - 1;
    testDate.tm_mday = day;
    testDate.tm_hour = 12;
    testDate.tm_min = 0;
    testDate.tm_sec = 0;
    testDate.tm_isdst = -1;

    time_t normalized = mktime(&testDate);
    if (normalized == static_cast<time_t>(-1)) {
        return false;
    }

    tm* check = localtime(&normalized);
    if (!check) {
        return false;
    }

    return (check->tm_year == testDate.tm_year &&
            check->tm_mon == testDate.tm_mon &&
            check->tm_mday == testDate.tm_mday);
}

static bool parseDateFlexible(const string& value, int& year, int& month, int& day) {
    if (parseDateYmd(value, year, month, day)) {
        return true;
    }

    // Fallback for older/manual inputs like DD/MM/YYYY or MM/DD/YYYY.
    if (value.size() == 10 && value[2] == '/' && value[5] == '/') {
        string p1 = value.substr(0, 2);
        string p2 = value.substr(3, 2);
        string p3 = value.substr(6, 4);
        if (isdigit(static_cast<unsigned char>(p1[0])) &&
            isdigit(static_cast<unsigned char>(p1[1])) &&
            isdigit(static_cast<unsigned char>(p2[0])) &&
            isdigit(static_cast<unsigned char>(p2[1])) &&
            isdigit(static_cast<unsigned char>(p3[0])) &&
            isdigit(static_cast<unsigned char>(p3[1])) &&
            isdigit(static_cast<unsigned char>(p3[2])) &&
            isdigit(static_cast<unsigned char>(p3[3]))) {
            int a = 0;
            int b = 0;
            int c = 0;
            try {
                a = stoi(p1);
                b = stoi(p2);
                c = stoi(p3);
            } catch (...) {
                return false;
            }

            // Heuristic:
            // - if first part > 12 => DD/MM/YYYY
            // - if second part > 12 => MM/DD/YYYY
            // - else default to DD/MM/YYYY
            if (a > 12) {
                day = a;
                month = b;
            } else if (b > 12) {
                month = a;
                day = b;
            } else {
                day = a;
                month = b;
            }
            year = c;
            return parseDateYmd(
                to_string(year) + "-" + (month < 10 ? "0" : "") + to_string(month) + "-" +
                (day < 10 ? "0" : "") + to_string(day),
                year, month, day);
        }
    }

    return false;
}

static string firstNonEmptyFormValue(const unordered_map<string, string>& form,
                                     initializer_list<const char*> keys) {
    for (const char* key : keys) {
        auto it = form.find(key);
        if (it != form.end()) {
            string value = trim(it->second);
            if (!value.empty()) {
                return value;
            }
        }
    }
    return "";
}

static string firstFormValue(const unordered_map<string, string>& form,
                             initializer_list<const char*> keys) {
    for (const char* key : keys) {
        auto it = form.find(key);
        if (it != form.end()) {
            return trim(it->second);
        }
    }
    return "";
}

static string firstLowerFormValue(const unordered_map<string, string>& form,
                                  initializer_list<const char*> keys) {
    return toLowerCopy(firstFormValue(form, keys));
}

static bool normalizeOnOffValue(const string& raw, bool defaultValue, bool& parsedValue) {
    string value = toLowerCopy(trim(raw));
    if (value.empty()) {
        parsedValue = defaultValue;
        return true;
    }
    if (value == "on" || value == "yes" || value == "true" || value == "1") {
        parsedValue = true;
        return true;
    }
    if (value == "off" || value == "no" || value == "false" || value == "0") {
        parsedValue = false;
        return true;
    }
    parsedValue = defaultValue;
    return false;
}

static bool fixedIntervalSeconds(int value, const string& unit, long long& seconds) {
    if (value <= 0) {
        return false;
    }

    if (unit == "minute") {
        seconds = static_cast<long long>(value) * 60LL;
        return true;
    }
    if (unit == "hour") {
        seconds = static_cast<long long>(value) * 3600LL;
        return true;
    }
    if (unit == "day") {
        seconds = static_cast<long long>(value) * 86400LL;
        return true;
    }
    if (unit == "week") {
        seconds = static_cast<long long>(value) * 604800LL;
        return true;
    }

    return false;
}

static long long addIntervalToEpoch(long long epochSeconds, int value, const string& unit) {
    long long seconds = 0;
    if (fixedIntervalSeconds(value, unit, seconds)) {
        return epochSeconds + seconds;
    }

    time_t rawTime = static_cast<time_t>(epochSeconds);
    tm* local = localtime(&rawTime);
    if (!local) {
        return epochSeconds;
    }

    tm next = *local;
    if (unit == "month") {
        next.tm_mon += value;
    } else if (unit == "year") {
        next.tm_year += value;
    } else {
        return epochSeconds;
    }

    next.tm_isdst = -1;
    time_t converted = mktime(&next);
    if (converted == static_cast<time_t>(-1)) {
        return epochSeconds;
    }

    return static_cast<long long>(converted);
}

static long long buildEpochFromLocalDateTime(int year, int month, int day, int timeHour, int timeMinute) {
    tm due{};
    due.tm_year = year - 1900;
    due.tm_mon = month - 1;
    due.tm_mday = day;
    due.tm_hour = timeHour;
    due.tm_min = timeMinute;
    due.tm_sec = 0;
    due.tm_isdst = -1;

    time_t converted = mktime(&due);
    if (converted == static_cast<time_t>(-1)) {
        return -1;
    }

    tm* check = localtime(&converted);
    if (!check) {
        return -1;
    }

    if (check->tm_year != due.tm_year || check->tm_mon != due.tm_mon || check->tm_mday != due.tm_mday ||
        check->tm_hour != due.tm_hour || check->tm_min != due.tm_min) {
        return -1;
    }

    return static_cast<long long>(converted);
}

static int weekdayToIndex(const string& tokenRaw) {
    const string token = toLowerCopy(trim(tokenRaw));
    if (token == "sun" || token == "sunday") return 0;
    if (token == "mon" || token == "monday") return 1;
    if (token == "tue" || token == "tues" || token == "tuesday") return 2;
    if (token == "wed" || token == "wednesday") return 3;
    if (token == "thu" || token == "thur" || token == "thurs" || token == "thursday") return 4;
    if (token == "fri" || token == "friday") return 5;
    if (token == "sat" || token == "saturday") return 6;
    return -1;
}

static vector<int> parseSelectedDaysCsv(const string& csv) {
    vector<int> result;
    bool used[7] = {false, false, false, false, false, false, false};
    size_t start = 0;
    while (start <= csv.size()) {
        size_t comma = csv.find(',', start);
        if (comma == string::npos) {
            comma = csv.size();
        }

        const string token = csv.substr(start, comma - start);
        const int day = weekdayToIndex(token);
        if (day >= 0 && day <= 6 && !used[day]) {
            used[day] = true;
            result.push_back(day);
        }

        if (comma == csv.size()) {
            break;
        }
        start = comma + 1;
    }

    sort(result.begin(), result.end());
    return result;
}

static string normalizeSelectedDaysCsv(const string& csv) {
    static const char* kDayNames[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    vector<int> days = parseSelectedDaysCsv(csv);
    ostringstream out;
    for (size_t i = 0; i < days.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << kDayNames[days[i]];
    }
    return out.str();
}

static string selectedDaysLabel(const string& csv) {
    static const char* kDayLabels[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    vector<int> days = parseSelectedDaysCsv(csv);
    if (days.empty()) {
        return "None";
    }

    ostringstream out;
    for (size_t i = 0; i < days.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << kDayLabels[days[i]];
    }
    return out.str();
}

static long long advanceToNextSelectedDay(long long dueEpochSeconds, const vector<int>& selectedDays) {
    if (selectedDays.empty()) {
        return dueEpochSeconds;
    }

    time_t rawTime = static_cast<time_t>(dueEpochSeconds);
    tm* local = localtime(&rawTime);
    if (!local) {
        return dueEpochSeconds;
    }
    tm base = *local;

    for (int offsetDays = 1; offsetDays <= 14; ++offsetDays) {
        tm candidate = base;
        candidate.tm_mday += offsetDays;
        candidate.tm_isdst = -1;
        time_t converted = mktime(&candidate);
        if (converted == static_cast<time_t>(-1)) {
            continue;
        }

        tm* check = localtime(&converted);
        if (!check) {
            continue;
        }

        const int weekday = check->tm_wday;
        if (find(selectedDays.begin(), selectedDays.end(), weekday) != selectedDays.end()) {
            return static_cast<long long>(converted);
        }
    }

    return dueEpochSeconds;
}

static long long advanceReminderDue(const DueReminder& reminder, long long dueEpochSeconds) {
    const string repeatMode = toLowerCopy(trim(reminder.repeatMode));
    if (repeatMode == "every_day") {
        return addIntervalToEpoch(dueEpochSeconds, 1, "day");
    }
    if (repeatMode == "every_x_days") {
        const int days = reminder.repeatEveryXDays > 0 ? reminder.repeatEveryXDays : 1;
        return addIntervalToEpoch(dueEpochSeconds, days, "day");
    }
    if (repeatMode == "selected_days") {
        return advanceToNextSelectedDay(dueEpochSeconds, parseSelectedDaysCsv(reminder.selectedDays));
    }

    // Backward-compatible fallback for old rows that only have repeat_value/repeat_unit.
    const string legacyUnit = toLowerCopy(trim(reminder.repeatUnit));
    if (reminder.repeatValue <= 0 || !isSupportedRepeatUnit(legacyUnit)) {
        return dueEpochSeconds;
    }
    return addIntervalToEpoch(dueEpochSeconds, reminder.repeatValue, legacyUnit);
}

static long long nextDueAfterCatchUp(const DueReminder& reminder, long long nowEpochSeconds, int& missedCount) {
    missedCount = 0;
    long long next = reminder.nextDueAt;
    const int kMaxSteps = 500000;
    while (next <= nowEpochSeconds && missedCount < kMaxSteps) {
        long long advanced = advanceReminderDue(reminder, next);
        if (advanced <= next) {
            break;
        }
        next = advanced;
        ++missedCount;
    }
    return next;
}

static long long nextDueAfterCatchUp(long long nextDueAt,
                                     const string& repeatMode,
                                     int repeatEveryXDays,
                                     const string& selectedDays,
                                     int legacyRepeatValue,
                                     const string& legacyRepeatUnit,
                                     long long nowEpochSeconds,
                                     int& missedCount) {
    DueReminder reminder;
    reminder.nextDueAt = nextDueAt;
    reminder.repeatMode = repeatMode;
    reminder.repeatEveryXDays = repeatEveryXDays;
    reminder.selectedDays = selectedDays;
    reminder.repeatValue = legacyRepeatValue;
    reminder.repeatUnit = legacyRepeatUnit;
    return nextDueAfterCatchUp(reminder, nowEpochSeconds, missedCount);
}

static bool isDurationExpired(const DueReminder& reminder, long long dueEpochSeconds) {
    const string durationType = toLowerCopy(trim(reminder.durationType));
    if (durationType != "number_of_days") {
        return false;
    }
    if (reminder.endAt <= 0) {
        return false;
    }
    return dueEpochSeconds > reminder.endAt;
}

static string repeatRuleLabel(const Medicine& med) {
    const string repeatMode = toLowerCopy(trim(med.getRepeatMode()));
    if (repeatMode == "every_day") {
        return "Every day";
    }
    if (repeatMode == "selected_days") {
        return "Selected days (" + selectedDaysLabel(med.getSelectedDays()) + ")";
    }
    if (repeatMode == "every_x_days") {
        const int days = med.getRepeatEveryXDays() > 0 ? med.getRepeatEveryXDays() : 1;
        return "Every " + to_string(days) + " day" + (days == 1 ? "" : "s");
    }

    // Backward-compatible label for old rows.
    int repeatValue = med.getRepeatValue();
    const string repeatUnit = toLowerCopy(trim(med.getRepeatUnit()));
    if (repeatValue <= 0) {
        return "One-time";
    }
    if (!isSupportedRepeatUnit(repeatUnit)) {
        return "Invalid repeat rule";
    }
    if (repeatValue == 1 && repeatUnit == "day") {
        return "Every day";
    }
    return "Every " + to_string(repeatValue) + " " + repeatUnit + (repeatValue == 1 ? "" : "s");
}

static string durationRuleLabel(const Medicine& med) {
    const string durationType = toLowerCopy(trim(med.getDurationType()));
    if (durationType == "number_of_days") {
        int days = med.getDurationDays();
        if (days < 1) {
            days = 1;
        }
        return "For " + to_string(days) + " day" + (days == 1 ? "" : "s");
    }
    return "Continuous";
}

static void processDueReminders(Database& reminderDb,
                                long long nowEpochSeconds,
                                unordered_map<int, vector<string>>& pendingNotifications,
                                const unordered_set<int>& activeAccountIds) {
    vector<DueReminder> dueReminders = reminderDb.loadDueReminders(nowEpochSeconds);
    for (const DueReminder& reminder : dueReminders) {
        if (isDurationExpired(reminder, reminder.nextDueAt)) {
            if (!reminderDb.disableReminder(reminder.medicineId)) {
                cerr << "Failed to disable expired reminder for medicine id " << reminder.medicineId << ".\n";
            }
            continue;
        }

        if (activeAccountIds.find(reminder.accountId) != activeAccountIds.end()) {
            const string scheduledAt = formatDateTime(reminder.nextDueAt);
            cout << "[REMINDER] Time to take your medicine | Name: " << reminder.name
                 << " | Dosage: " << reminder.dosage
                 << " | Scheduled: " << scheduledAt;
            if (!reminder.timeLabel.empty()) {
                cout << " | Time: " << reminder.timeLabel;
            }
            cout << "\n";

            string userMessage = "Time to take medicine: " + reminder.name +
                                 " (" + reminder.dosage + "). Scheduled at " + scheduledAt + ".";
            vector<string>& queue = pendingNotifications[reminder.accountId];
            queue.push_back(userMessage);
            if (queue.size() > 20) {
                queue.erase(queue.begin());
            }
        }

        int missedCount = 0;
        long long nextDueAt = nextDueAfterCatchUp(reminder, nowEpochSeconds, missedCount);
        if (nextDueAt <= reminder.nextDueAt) {
            if (!reminderDb.disableReminder(reminder.medicineId)) {
                cerr << "Failed to disable reminder for medicine id " << reminder.medicineId << ".\n";
            }
            continue;
        }

        if (isDurationExpired(reminder, nextDueAt)) {
            if (!reminderDb.disableReminder(reminder.medicineId)) {
                cerr << "Failed to disable expired reminder for medicine id " << reminder.medicineId << ".\n";
            }
            continue;
        }

        if (nextDueAt <= reminder.nextDueAt || !reminderDb.updateReminderNextDue(reminder.medicineId, nextDueAt)) {
            cerr << "Failed to move next_due_at for medicine id " << reminder.medicineId << ".\n";
        }
    }
}

static int accountIdFromSession(const HttpRequest& req, const unordered_map<string, int>& sessions) {
    auto it = req.headers.find("cookie");
    if (it == req.headers.end()) {
        return -1;
    }

    string token = getCookieValue(it->second, "SESSION_ID");
    if (token.empty()) {
        return -1;
    }

    auto sIt = sessions.find(token);
    if (sIt == sessions.end()) {
        return -1;
    }

    return sIt->second;
}

static bool isProtectedPath(const string& path) {
    return path == "/dashboard.html" ||
           path == "/add-medicine.html" ||
           path == "/medicine-history.html" ||
           path == "/settings.html" ||
           path == "/user-profile.html";
}

static bool isAllowedStaticPath(const string& path) {
    static const unordered_set<string> allowed = {
        "/",
        "/login.html",
        "/create-account.html",
        "/add-medicine.html",
        "/common.css",
        "/login.css",
        "/create-account.css",
        "/add-medicine.css",
        "/dashboard.css",
        "/medicine-history.css",
        "/setting.css",
        "/style.css"
    };

    return allowed.find(path) != allowed.end();
}

static bool userOwnsMedicine(Database& db, int accountId, int medicineId) {
    vector<Medicine> medicines = db.loadMedicines(accountId);
    for (const auto& med : medicines) {
        if (med.getId() == medicineId) {
            return true;
        }
    }
    return false;
}

static string renderMessagePage(const string& title,
                                const string& message,
                                const string& backHref,
                                const string& backText) {
    ostringstream html;
    html << "<!DOCTYPE html><html><head>"
         << "<meta charset=\"UTF-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
         << "<title>" << htmlEscape(title) << "</title>"
         << "<link rel=\"stylesheet\" href=\"/common.css\">"
         << "<link rel=\"stylesheet\" href=\"/setting.css\">"
         << "</head><body>"
         << "<header class=\"app-header\"><h1>" << htmlEscape(title) << "</h1></header>"
         << "<main class=\"container\"><section class=\"card text-center\">"
         << "<p>" << htmlEscape(message) << "</p>"
         << "<a class=\"btn btn-secondary btn-block mt-md\" href=\"" << htmlEscape(backHref) << "\">" << htmlEscape(backText) << "</a>"
         << "</section></main></body></html>";
    return html.str();
}

static string renderDashboardPage(const string& username) {
    ostringstream html;
    html << "<!DOCTYPE html><html lang=\"en\"><head>"
         << "<meta charset=\"UTF-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
         << "<title>Dashboard</title>"
         << "<link rel=\"stylesheet\" href=\"/common.css\">"
         << "<link rel=\"stylesheet\" href=\"/dashboard.css\">"
         << "</head><body>"
         << "<header class=\"app-header\"><h1>Medicine Reminder</h1><p>Welcome, " << htmlEscape(username) << "</p></header>"
         << "<main><div class=\"cards-container\">"
         << "<a class=\"card dash-card\" href=\"/add-medicine.html\"><h2>Add Medicine</h2></a>"
         << "<a class=\"card dash-card\" href=\"/medicine-history.html\"><h2>Medicine History</h2></a>"
         << "<a class=\"card dash-card\" href=\"/user-profile.html\"><h2>User Profile</h2></a>"
         << "<a class=\"card dash-card\" href=\"/settings.html\"><h2>Settings</h2></a>"
         << "</div></main>"
         << "<footer><p><a href=\"/logout\">Logout</a></p></footer>"
         << "</body></html>";
    return html.str();
}

static string renderProfilePage(const string& username, const User& user) {
    const bool hasPhoto = !user.profilePhotoPath.empty();
    ostringstream html;
    html << "<!DOCTYPE html><html lang=\"en\"><head>"
         << "<meta charset=\"UTF-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
         << "<title>User Profile</title>"
         << "<link rel=\"stylesheet\" href=\"/common.css\">"
         << "<link rel=\"stylesheet\" href=\"/style.css\">"
         << "</head><body>"
         << "<header class=\"top-nav\">"
         << "<a href=\"/dashboard.html\">Dashboard</a>"
         << "<a href=\"/settings.html\">Settings</a>"
         << "</header>"
         << "<main>"
         << "<h1 class=\"page-title\">User Profile</h1>"
         << "<section class=\"card center\">"
         << "<h2>Profile Picture</h2>"
         << "<div class=\"profile-circle\">";
    if (hasPhoto) {
        html << "<img class=\"profile-photo\" src=\"/profile-photo\" alt=\"Profile Photo\">";
    } else {
        html << "<div class=\"cute-avatar\">U</div>";
    }
    html << "</div>"
         << "<form method=\"post\" action=\"/profile/photo\" enctype=\"multipart/form-data\">"
         << "<input type=\"file\" name=\"photo\" accept=\".jpg,.jpeg,.png,.webp\" required>"
         << "<button class=\"save-btn\" type=\"submit\">Upload Profile Photo</button>"
         << "</form>"
         << "<p>Logged in as: <code>" << htmlEscape(username) << "</code></p>"
         << "</section>"
         << "<section class=\"card\">"
         << "<form method=\"post\" action=\"/profile/update\">"
         << "<label for=\"profile-name\">Name</label>"
         << "<input id=\"profile-name\" type=\"text\" name=\"name\" value=\"" << htmlEscape(user.name) << "\" required>"
         << "<label for=\"profile-age\">Age</label>"
         << "<input id=\"profile-age\" type=\"number\" name=\"age\" min=\"1\" value=\""
         << (user.age > 0 ? to_string(user.age) : "") << "\" required>"
         << "<label for=\"profile-gender\">Gender</label>"
         << "<input id=\"profile-gender\" type=\"text\" name=\"gender\" value=\"" << htmlEscape(user.gender) << "\" required>"
         << "<label for=\"profile-medical\">Medical Condition</label>"
         << "<input id=\"profile-medical\" type=\"text\" name=\"medical_issue\" value=\"" << htmlEscape(user.medicalIssue) << "\">"
         << "<label for=\"profile-contact\">Contact Number</label>"
         << "<input id=\"profile-contact\" type=\"text\" name=\"contact_number\" value=\"" << htmlEscape(user.contactNumber) << "\" required>"
         << "<label for=\"profile-emergency\">Emergency Contact</label>"
         << "<input id=\"profile-emergency\" type=\"text\" name=\"emergency_contact\" value=\"" << htmlEscape(user.emergencyContact) << "\">"
         << "<button class=\"save-btn\" type=\"submit\">Save Profile</button>"
         << "</form>"
         << "</section>"
         << "<section class=\"card center\">"
         << "<a class=\"logout-btn\" href=\"/logout\">Logout</a>"
         << "</section>"
         << "</main>"
         << "</body></html>";
    return html.str();
}

static string renderSettingsPage(const string& username) {
    ostringstream html;
    html << "<!DOCTYPE html><html lang=\"en\"><head>"
         << "<meta charset=\"UTF-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
         << "<title>Settings</title>"
         << "<link rel=\"stylesheet\" href=\"/common.css\">"
         << "<link rel=\"stylesheet\" href=\"/setting.css\">"
         << "</head><body>"
         << "<header class=\"app-header\"><h1>Settings</h1><p>Manage account details</p></header>"
         << "<main>"
         << "<section class=\"card\">"
         << "<h2>Change Username</h2>"
         << "<form method=\"post\" action=\"/settings/username\">"
         << "<input type=\"text\" name=\"new_username\" value=\"" << htmlEscape(username) << "\" required>"
         << "<button class=\"btn btn-primary btn-block\" type=\"submit\">Update Username</button>"
         << "</form>"
         << "</section>"
         << "<section class=\"card\">"
         << "<h2>Change Password</h2>"
         << "<form method=\"post\" action=\"/settings/password\">"
         << "<input type=\"password\" name=\"old_password\" placeholder=\"Old password\" required>"
         << "<input type=\"password\" name=\"new_password\" placeholder=\"New password\" required>"
         << "<button class=\"btn btn-primary btn-block\" type=\"submit\">Update Password</button>"
         << "</form>"
         << "</section>"
         << "<section class=\"card text-center\"><a class=\"btn btn-secondary btn-block\" href=\"/dashboard.html\">Back to Dashboard</a></section>"
         << "</main>"
         << "<footer class=\"app-footer\"><p>Medicine Reminder</p></footer>"
         << "</body></html>";
    return html.str();
}

static string renderHistoryPage(Database& db, int accountId) {
    vector<Medicine> medicines = db.loadMedicines(accountId);

    ostringstream html;
    html << "<!DOCTYPE html><html lang=\"en\"><head>"
         << "<meta charset=\"UTF-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
         << "<title>Medicine History</title>"
         << "<link rel=\"stylesheet\" href=\"/common.css\">"
         << "<link rel=\"stylesheet\" href=\"/medicine-history.css\">"
         << "</head><body>"
         << "<header class=\"app-header\" style=\"display:flex;justify-content:space-between;align-items:center;text-align:left;\">"
         << "<h1>Medicine History</h1><a class=\"settings-btn\" href=\"/settings.html\">Settings</a></header>"
         << "<main><section id=\"historyContainer\">";

    if (medicines.empty()) {
        html << "<div class=\"card\"><h3>No medicines yet</h3><p>Add medicines to build your history.</p></div>";
    } else {
        for (size_t i = 0; i < medicines.size(); ++i) {
            const Medicine& med = medicines[i];
            html << "<div class=\"card\">"
                 << "<h3>" << (i + 1) << ". " << htmlEscape(med.getName()) << "</h3>"
                 << "<p>Dosage: " << htmlEscape(med.getDosage()) << "</p>"
                 << "<p>Time: " << htmlEscape(med.getTime()) << "</p>"
                 << "<p>Next Due: " << htmlEscape(formatDateTime(med.getNextDueAt())) << "</p>"
                 << "<p>Repeat: " << htmlEscape(repeatRuleLabel(med)) << "</p>"
                 << "<p>Start Date: " << htmlEscape(med.getStartDate().empty() ? "Not set" : med.getStartDate()) << "</p>"
                 << "<p>Duration: " << htmlEscape(durationRuleLabel(med)) << "</p>"
                 << "<p>Reminder: " << (med.isEnabled() ? "Enabled" : "Disabled") << "</p>"
                 << "<p>Status: " << (med.isTaken() ? "Taken" : "Pending") << "</p>";

            if (!med.isTaken()) {
                html << "<form method=\"post\" action=\"/take-medicine\">"
                     << "<input type=\"hidden\" name=\"medicine_id\" value=\"" << med.getId() << "\">"
                     << "<button class=\"btn btn-primary btn-sm\" type=\"submit\">Mark Taken</button>"
                     << "</form>";
            }

            html << "<form method=\"post\" action=\"/delete-medicine\">"
                 << "<input type=\"hidden\" name=\"medicine_id\" value=\"" << med.getId() << "\">"
                 << "<button class=\"btn btn-danger btn-sm\" type=\"submit\">Delete</button>"
                 << "</form>"
                 << "</div>";
        }
    }

    html << "</section></main>"
         << "<footer class=\"bottom-nav\">"
         << "<a class=\"btn btn-primary\" href=\"/dashboard.html\">Dashboard</a>"
         << "<a class=\"btn btn-primary\" href=\"/add-medicine.html\">Add Medicine</a>"
         << "<a class=\"btn btn-primary\" href=\"/user-profile.html\">User Profile</a>"
         << "</footer>"
         << "</body></html>";

    return html.str();
}

static void serveStaticFile(SOCKET client, const string& path) {
    string safePath = path;
    if (safePath == "/") {
        safePath = "/login.html";
    }

    if (!isAllowedStaticPath(safePath)) {
        sendResponse(client, 404, "Not Found", "text/plain; charset=utf-8", "File not found");
        return;
    }

    if (safePath.find("..") != string::npos || safePath.find(':') != string::npos) {
        sendResponse(client, 400, "Bad Request", "text/plain; charset=utf-8", "Invalid path");
        return;
    }

    if (!safePath.empty() && safePath.front() == '/') {
        safePath.erase(safePath.begin());
    }

    string data;
    if (!readFile(safePath, data)) {
        sendResponse(client, 404, "Not Found", "text/plain; charset=utf-8", "File not found");
        return;
    }

    sendResponse(client, 200, "OK", getMimeType(safePath), data);
}

int main() {
    Database db("medicine_reminder.db");
    if (!db.isOpen()) {
        cerr << "Database connection failed.\n";
        return 1;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cerr << "WSAStartup failed.\n";
        return 1;
    }

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        cerr << "Could not create socket.\n";
        WSACleanup();
        return 1;
    }

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        cerr << "Bind failed. Port 8080 may already be in use.\n";
        closesocket(server);
        WSACleanup();
        return 1;
    }

    if (listen(server, SOMAXCONN) == SOCKET_ERROR) {
        cerr << "Listen failed.\n";
        closesocket(server);
        WSACleanup();
        return 1;
    }

    unordered_map<string, int> sessions;
    unordered_map<int, vector<string>> pendingNotifications;
    Database reminderDb("medicine_reminder.db");
    if (!reminderDb.isOpen()) {
        cerr << "Reminder scheduler could not open database.\n";
    } else {
        vector<MissedReminderInfo> missed = reminderDb.catchUpMissedReminders(currentEpochSeconds());
        if (!missed.empty()) {
            cout << "[REMINDER] Missed reminders while server was offline:\n";
            for (const MissedReminderInfo& info : missed) {
                cout << "  - " << info.name << ": " << info.missedCount
                     << " missed, next due at " << formatDateTime(info.nextDueAt) << "\n";
            }
        }
    }
    long long nextReminderCheckAt = currentEpochSeconds();

    cout << "Server running at http://localhost:8080\n";
    cout << "Press Ctrl+C to stop.\n";

    while (true) {
        const long long nowEpochSeconds = currentEpochSeconds();
        if (reminderDb.isOpen() && nowEpochSeconds >= nextReminderCheckAt) {
            unordered_set<int> activeAccountIds;
            for (const auto& session : sessions) {
                activeAccountIds.insert(session.second);
            }

            // Keep in-memory notifications only for currently logged-in accounts.
            for (auto it = pendingNotifications.begin(); it != pendingNotifications.end();) {
                if (activeAccountIds.find(it->first) == activeAccountIds.end()) {
                    it = pendingNotifications.erase(it);
                } else {
                    ++it;
                }
            }

            processDueReminders(reminderDb, nowEpochSeconds, pendingNotifications, activeAccountIds);
            nextReminderCheckAt = nowEpochSeconds + 1LL;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(server, &readSet);

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR) {
            continue;
        }

        if (ready == 0) {
            continue;
        }

        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }

        string raw;
        if (!readHttpRequest(client, raw)) {
            closesocket(client);
            continue;
        }

        HttpRequest req = parseHttpRequest(raw);
        if (req.method.empty() || req.path.empty()) {
            sendResponse(client, 400, "Bad Request", "text/plain; charset=utf-8", "Invalid request");
            closesocket(client);
            continue;
        }

        int accountId = accountIdFromSession(req, sessions);

        if (req.method == "GET" && req.path == "/api/notifications") {
            if (accountId < 0) {
                sendResponse(client, 401, "Unauthorized", "application/json; charset=utf-8",
                             "{\"notifications\":[]}");
                closesocket(client);
                continue;
            }

            ostringstream json;
            json << "{\"notifications\":[";
            auto it = pendingNotifications.find(accountId);
            if (it != pendingNotifications.end()) {
                for (size_t i = 0; i < it->second.size(); ++i) {
                    if (i > 0) {
                        json << ",";
                    }
                    json << "\"" << jsonEscape(it->second[i]) << "\"";
                }
                it->second.clear();
            }
            json << "]}";

            sendResponse(client, 200, "OK", "application/json; charset=utf-8", json.str());
            closesocket(client);
            continue;
        }

        if (req.method == "GET" && req.path == "/logout") {
            auto it = req.headers.find("cookie");
            if (it != req.headers.end()) {
                string token = getCookieValue(it->second, "SESSION_ID");
                if (!token.empty()) {
                    sessions.erase(token);
                }
            }

            sendRedirect(client, "/login.html", {{"Set-Cookie", "SESSION_ID=deleted; Path=/; Max-Age=0; HttpOnly"}});
            closesocket(client);
            continue;
        }

        if (req.method == "GET" && req.path == "/dashboard.html") {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
            } else {
                string username;
                string password;
                if (!db.getAccountById(accountId, username, password)) {
                    sendRedirect(client, "/logout");
                } else {
                    sendResponse(client, 200, "OK", "text/html; charset=utf-8", renderDashboardPage(username));
                }
            }
            closesocket(client);
            continue;
        }

        if (req.method == "GET" && req.path == "/user-profile.html") {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
            } else {
                string username;
                string password;
                if (!db.getAccountById(accountId, username, password)) {
                    sendRedirect(client, "/logout");
                } else {
                    User user;
                    db.loadUser(accountId, user);
                    sendResponse(client, 200, "OK", "text/html; charset=utf-8", renderProfilePage(username, user));
                }
            }
            closesocket(client);
            continue;
        }

        if (req.method == "GET" && req.path == "/profile-photo") {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
                closesocket(client);
                continue;
            }

            string photoPath;
            if (!db.getUserProfilePhotoPath(accountId, photoPath) ||
                photoPath.empty() ||
                !isSafeStoredProfilePhotoPath(photoPath)) {
                sendResponse(client, 404, "Not Found", "text/plain; charset=utf-8", "No profile photo");
                closesocket(client);
                continue;
            }

            string data;
            if (!readFile(photoPath, data)) {
                sendResponse(client, 404, "Not Found", "text/plain; charset=utf-8", "No profile photo");
                closesocket(client);
                continue;
            }

            sendResponse(client, 200, "OK", getMimeType(photoPath), data);
            closesocket(client);
            continue;
        }

        if (req.method == "GET" && req.path == "/settings.html") {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
            } else {
                string username;
                string password;
                if (!db.getAccountById(accountId, username, password)) {
                    sendRedirect(client, "/logout");
                } else {
                    sendResponse(client, 200, "OK", "text/html; charset=utf-8", renderSettingsPage(username));
                }
            }
            closesocket(client);
            continue;
        }

        if (req.method == "GET" && req.path == "/medicine-history.html") {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
            } else {
                sendResponse(client, 200, "OK", "text/html; charset=utf-8", renderHistoryPage(db, accountId));
            }
            closesocket(client);
            continue;
        }

        if (req.method == "GET" && req.path == "/add-medicine") {
            sendRedirect(client, "/add-medicine.html");
            closesocket(client);
            continue;
        }

        if (req.method == "POST" && req.path == "/signup") {
            unordered_map<string, string> form = parseForm(req.body);
            string username = trim(form["username"]);
            string password = trim(form["password"]);

            if (username.empty() || password.empty()) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Signup Error", "Username and password are required.",
                                               "/create-account.html", "Back"));
                closesocket(client);
                continue;
            }

            int newAccountId = -1;
            if (!db.createAccount(username, password, newAccountId)) {
                sendResponse(client, 409, "Conflict", "text/html; charset=utf-8",
                             renderMessagePage("Signup Error", "Account creation failed. Username may already exist.",
                                               "/create-account.html", "Try Again"));
                closesocket(client);
                continue;
            }

            string token = generateSessionToken();
            sessions[token] = newAccountId;
            sendRedirect(client, "/dashboard.html", {{"Set-Cookie", "SESSION_ID=" + token + "; Path=/; HttpOnly"}});
            closesocket(client);
            continue;
        }

        if (req.method == "POST" && req.path == "/login") {
            unordered_map<string, string> form = parseForm(req.body);
            string username = trim(form["username"]);
            string password = trim(form["password"]);
            int loggedInAccountId = -1;

            if (!db.login(username, password, loggedInAccountId)) {
                sendResponse(client, 401, "Unauthorized", "text/html; charset=utf-8",
                             renderMessagePage("Login Error", "Invalid username or password.",
                                               "/login.html", "Try Again"));
                closesocket(client);
                continue;
            }

            string token = generateSessionToken();
            sessions[token] = loggedInAccountId;
            sendRedirect(client, "/dashboard.html", {{"Set-Cookie", "SESSION_ID=" + token + "; Path=/; HttpOnly"}});
            closesocket(client);
            continue;
        }

        if (req.method == "POST" && req.path == "/profile/update") {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
                closesocket(client);
                continue;
            }

            unordered_map<string, string> form = parseForm(req.body);
            User user;
            user.name = trim(form["name"]);
            user.gender = trim(form["gender"]);
            user.medicalIssue = trim(form["medical_issue"]);
            user.contactNumber = trim(form["contact_number"]);
            user.emergencyContact = trim(form["emergency_contact"]);

            try {
                user.age = stoi(trim(form["age"]));
            } catch (...) {
                user.age = 0;
            }

            if (user.name.empty() || user.gender.empty() || user.contactNumber.empty() || user.age <= 0) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Profile Error", "Name, age, gender and contact are required.",
                                               "/user-profile.html", "Back"));
                closesocket(client);
                continue;
            }

            if (!db.saveUser(accountId, user)) {
                sendResponse(client, 500, "Internal Server Error", "text/html; charset=utf-8",
                             renderMessagePage("Profile Error", "Failed to save profile.",
                                               "/user-profile.html", "Back"));
                closesocket(client);
                continue;
            }

            sendRedirect(client, "/user-profile.html");
            closesocket(client);
            continue;
        }

        if (req.method == "POST" && req.path == "/profile/photo") {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
                closesocket(client);
                continue;
            }

            auto contentTypeIt = req.headers.find("content-type");
            string boundary;
            if (contentTypeIt == req.headers.end() || !extractMultipartBoundary(contentTypeIt->second, boundary)) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Profile Error", "Invalid upload request.",
                                               "/user-profile.html", "Back"));
                closesocket(client);
                continue;
            }

            MultipartFilePart filePart;
            if (!parseMultipartFileField(req.body, boundary, "photo", filePart)) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Profile Error", "Please choose an image file to upload.",
                                               "/user-profile.html", "Back"));
                closesocket(client);
                continue;
            }

            static const size_t kMaxPhotoBytes = 2U * 1024U * 1024U;
            if (filePart.data.empty() || filePart.data.size() > kMaxPhotoBytes) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Profile Error", "Photo must be between 1 byte and 2 MB.",
                                               "/user-profile.html", "Back"));
                closesocket(client);
                continue;
            }

            string photoExt;
            string photoMime;
            if (!inferPhotoFormat(filePart, photoExt, photoMime)) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Profile Error", "Only PNG, JPG, or WEBP images are allowed.",
                                               "/user-profile.html", "Back"));
                closesocket(client);
                continue;
            }

            if (!ensureProfileUploadsDirectory()) {
                sendResponse(client, 500, "Internal Server Error", "text/html; charset=utf-8",
                             renderMessagePage("Profile Error", "Failed to prepare upload directory.",
                                               "/user-profile.html", "Back"));
                closesocket(client);
                continue;
            }

            string previousPhotoPath;
            db.getUserProfilePhotoPath(accountId, previousPhotoPath);

            const string newPhotoPath =
                "uploads/profile/acc_" + to_string(accountId) + "_" + generateSessionToken() + photoExt;
            if (!writeFile(newPhotoPath, filePart.data)) {
                sendResponse(client, 500, "Internal Server Error", "text/html; charset=utf-8",
                             renderMessagePage("Profile Error", "Failed to save uploaded file.",
                                               "/user-profile.html", "Back"));
                closesocket(client);
                continue;
            }

            if (!db.updateUserProfilePhotoPath(accountId, newPhotoPath)) {
                std::remove(newPhotoPath.c_str());
                sendResponse(client, 500, "Internal Server Error", "text/html; charset=utf-8",
                             renderMessagePage("Profile Error", "Failed to save profile photo in database.",
                                               "/user-profile.html", "Back"));
                closesocket(client);
                continue;
            }

            if (isSafeStoredProfilePhotoPath(previousPhotoPath) && previousPhotoPath != newPhotoPath) {
                std::remove(previousPhotoPath.c_str());
            }

            sendRedirect(client, "/user-profile.html");
            closesocket(client);
            continue;
        }

        if (req.method == "POST" && req.path == "/settings/username") {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
                closesocket(client);
                continue;
            }

            unordered_map<string, string> form = parseForm(req.body);
            string newUsername = trim(form["new_username"]);
            if (newUsername.empty()) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Settings Error", "Username cannot be empty.",
                                               "/settings.html", "Back"));
                closesocket(client);
                continue;
            }

            if (!db.updateUsername(accountId, newUsername)) {
                sendResponse(client, 409, "Conflict", "text/html; charset=utf-8",
                             renderMessagePage("Settings Error", "Failed to update username (maybe already used).",
                                               "/settings.html", "Back"));
                closesocket(client);
                continue;
            }

            sendRedirect(client, "/settings.html");
            closesocket(client);
            continue;
        }

        if (req.method == "POST" && req.path == "/settings/password") {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
                closesocket(client);
                continue;
            }

            unordered_map<string, string> form = parseForm(req.body);
            string oldPassword = trim(form["old_password"]);
            string newPassword = trim(form["new_password"]);

            if (oldPassword.empty() || newPassword.empty()) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Settings Error", "Both old and new password are required.",
                                               "/settings.html", "Back"));
                closesocket(client);
                continue;
            }

            if (!db.verifyAccountPassword(accountId, oldPassword)) {
                sendResponse(client, 401, "Unauthorized", "text/html; charset=utf-8",
                             renderMessagePage("Settings Error", "Old password is incorrect.",
                                               "/settings.html", "Back"));
                closesocket(client);
                continue;
            }

            if (!db.updatePassword(accountId, newPassword)) {
                sendResponse(client, 500, "Internal Server Error", "text/html; charset=utf-8",
                             renderMessagePage("Settings Error", "Failed to update password.",
                                               "/settings.html", "Back"));
                closesocket(client);
                continue;
            }

            sendRedirect(client, "/settings.html");
            closesocket(client);
            continue;
        }

        if (req.method == "POST" && req.path == "/add-medicine") {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
                closesocket(client);
                continue;
            }

            unordered_map<string, string> form = parseForm(req.body);
            string name = firstNonEmptyFormValue(form, {"name", "medicine_name"});
            string dosage = firstNonEmptyFormValue(form, {"dosage", "dose"});
            string time24h = firstNonEmptyFormValue(form, {"time_24h", "time"});
            string enableReminderRaw = firstLowerFormValue(form, {"enable_reminder", "repeat_enabled"});
            string frequency = "specific_times";
            string repeatMode = firstLowerFormValue(form, {"repeat", "repeat_mode"});
            string selectedDays = normalizeSelectedDaysCsv(firstFormValue(form, {"selected_days", "repeat_days"}));
            string repeatEveryXDaysRaw = firstFormValue(form, {"repeat_every_x_days", "every_x_days"});
            string startDate = firstNonEmptyFormValue(form, {"start_date", "first_reminder_date", "date"});
            string durationType = firstLowerFormValue(form, {"duration_type"});
            string durationDaysRaw = firstFormValue(form, {"duration_days", "number_of_days"});

            int timeHour = -1;
            int timeMinute = -1;
            bool validTime = parseTime24(time24h, timeHour, timeMinute);
            int dateYear = 0;
            int dateMonth = 0;
            int dateDay = 0;
            bool validDate = parseDateFlexible(startDate, dateYear, dateMonth, dateDay);

            string validationError;
            if (name.empty()) {
                validationError = "Medicine name is required.";
            } else if (dosage.empty()) {
                validationError = "Dosage is required.";
            } else if (!validTime) {
                validationError = "Time must be in HH:MM (24-hour) format.";
            } else if (!validDate) {
                validationError = "Start date is invalid. Use YYYY-MM-DD.";
            }

            if (!validationError.empty()) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Medicine Error", validationError,
                                               "/add-medicine.html", "Back"));
                closesocket(client);
                continue;
            }

            bool enableReminder = true;
            normalizeOnOffValue(enableReminderRaw, true, enableReminder);

            if (repeatMode.empty() || !isSupportedRepeatMode(repeatMode)) {
                repeatMode = "every_day";
            }
            if (durationType.empty() || !isSupportedDurationType(durationType)) {
                durationType = "continuous";
            }

            int repeatEveryXDays = 1;
            if (repeatMode == "every_x_days") {
                try {
                    repeatEveryXDays = stoi(repeatEveryXDaysRaw);
                } catch (...) {
                    repeatEveryXDays = 1;
                }
                if (repeatEveryXDays <= 0) {
                    repeatEveryXDays = 1;
                }
            }

            int durationDays = 0;
            if (durationType == "number_of_days") {
                try {
                    durationDays = stoi(durationDaysRaw);
                } catch (...) {
                    durationDays = 1;
                }
                if (durationDays <= 0) {
                    durationDays = 1;
                }
            } else {
                durationDays = 0;
            }

            if (repeatMode == "selected_days" && parseSelectedDaysCsv(selectedDays).empty()) {
                // Fallback to a safe default instead of rejecting request.
                selectedDays = "mon";
            }

            int repeatValue = 1;
            string repeatUnit = "day";
            int repeatMinutes = 1440;
            if (repeatMode == "every_x_days") {
                repeatValue = repeatEveryXDays;
                repeatUnit = "day";
                long long repeatMinutes64 = static_cast<long long>(repeatEveryXDays) * 1440LL;
                if (repeatMinutes64 > static_cast<long long>(numeric_limits<int>::max())) {
                    sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                                 renderMessagePage("Medicine Error", "Repeat interval is too large.",
                                                   "/add-medicine.html", "Back"));
                    closesocket(client);
                    continue;
                }
                repeatMinutes = static_cast<int>(repeatMinutes64);
            } else if (repeatMode == "selected_days") {
                repeatValue = 1;
                repeatUnit = "week";
                repeatMinutes = 10080;
            }

            long long nowEpochSeconds = currentEpochSeconds();
            long long nextDueAt = buildEpochFromLocalDateTime(dateYear, dateMonth, dateDay, timeHour, timeMinute);
            if (nextDueAt <= 0) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Medicine Error", "Invalid start date/time.",
                                               "/add-medicine.html", "Back"));
                closesocket(client);
                continue;
            }

            long long endAt = 0;
            if (durationType == "number_of_days") {
                long long endExclusive = addIntervalToEpoch(nextDueAt, durationDays, "day");
                if (endExclusive <= nextDueAt) {
                    sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                                 renderMessagePage("Medicine Error", "Invalid duration range.",
                                                   "/add-medicine.html", "Back"));
                    closesocket(client);
                    continue;
                }
                endAt = endExclusive - 1;
            }

            if (enableReminder && nextDueAt <= nowEpochSeconds) {
                // If user picks the current minute, treat it as "due now" instead of skipping to next cycle.
                if ((nowEpochSeconds - nextDueAt) < 60LL) {
                    nextDueAt = nowEpochSeconds;
                } else {
                    int ignoredMissedCount = 0;
                    nextDueAt = nextDueAfterCatchUp(nextDueAt,
                                                    repeatMode,
                                                    repeatEveryXDays,
                                                    selectedDays,
                                                    repeatValue,
                                                    repeatUnit,
                                                    nowEpochSeconds,
                                                    ignoredMissedCount);
                }
            }

            if (enableReminder && nextDueAt <= nowEpochSeconds) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Medicine Error", "Start date/time must produce a future reminder.",
                                               "/add-medicine.html", "Back"));
                closesocket(client);
                continue;
            }

            if (enableReminder && endAt > 0 && nextDueAt > endAt) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Medicine Error", "Reminder duration has already ended.",
                                               "/add-medicine.html", "Back"));
                closesocket(client);
                continue;
            }

            ostringstream timeLabel;
            timeLabel << (timeHour < 10 ? "0" : "") << timeHour
                      << ":" << (timeMinute < 10 ? "0" : "") << timeMinute;

            Medicine med(name,
                         dosage,
                         timeLabel.str(),
                         false,
                         -1,
                         nextDueAt,
                         repeatMinutes,
                         repeatValue,
                         repeatUnit,
                         enableReminder,
                         frequency,
                         repeatMode,
                         repeatEveryXDays,
                         selectedDays,
                         startDate,
                         durationType,
                         durationDays,
                         endAt);
            if (db.addMedicine(accountId, med) == -1) {
                sendResponse(client, 500, "Internal Server Error", "text/html; charset=utf-8",
                             renderMessagePage("Medicine Error", "Failed to save medicine.",
                                               "/add-medicine.html", "Back"));
                closesocket(client);
                continue;
            }

            {
                vector<string>& queue = pendingNotifications[accountId];
                queue.push_back("Medicine saved: " + name + " (" + dosage + "). First reminder at " +
                                formatDateTime(nextDueAt) + ".");
                if (queue.size() > 20) {
                    queue.erase(queue.begin());
                }
            }

            sendRedirect(client, "/medicine-history.html");
            closesocket(client);
            continue;
        }

        if (req.method == "POST" && (req.path == "/take-medicine" || req.path == "/delete-medicine")) {
            if (accountId < 0) {
                sendRedirect(client, "/login.html");
                closesocket(client);
                continue;
            }

            unordered_map<string, string> form = parseForm(req.body);
            int medicineId = 0;
            try {
                medicineId = stoi(form["medicine_id"]);
            } catch (...) {
                medicineId = 0;
            }

            if (medicineId <= 0 || !userOwnsMedicine(db, accountId, medicineId)) {
                sendResponse(client, 400, "Bad Request", "text/html; charset=utf-8",
                             renderMessagePage("Medicine Error", "Invalid medicine selection.",
                                               "/medicine-history.html", "Back"));
                closesocket(client);
                continue;
            }

            bool ok = false;
            if (req.path == "/take-medicine") {
                ok = db.markMedicineTaken(medicineId);
            } else {
                ok = db.deleteMedicine(medicineId);
            }

            if (!ok) {
                sendResponse(client, 500, "Internal Server Error", "text/html; charset=utf-8",
                             renderMessagePage("Medicine Error", "Operation failed.",
                                               "/medicine-history.html", "Back"));
                closesocket(client);
                continue;
            }

            sendRedirect(client, "/medicine-history.html");
            closesocket(client);
            continue;
        }

        if (req.method == "GET") {
            if (isProtectedPath(req.path) && accountId < 0) {
                sendRedirect(client, "/login.html");
            } else {
                serveStaticFile(client, req.path);
            }
            closesocket(client);
            continue;
        }

        sendResponse(client, 405, "Method Not Allowed", "text/plain; charset=utf-8", "Method not allowed");
        closesocket(client);
    }

    closesocket(server);
    WSACleanup();
    return 0;
}
