// Database.cpp
#include "Database.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

static std::string lowerCopy(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static bool isSupportedRepeatUnit(const std::string& unit) {
    return unit == "minute" || unit == "hour" || unit == "day" || unit == "week" ||
           unit == "month" || unit == "year";
}

static bool isSupportedRepeatMode(const std::string& repeatMode) {
    return repeatMode == "every_day" || repeatMode == "selected_days" || repeatMode == "every_x_days";
}

static bool isSupportedDurationType(const std::string& durationType) {
    return durationType == "continuous" || durationType == "number_of_days";
}

static std::string trimCopy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

static bool fixedIntervalSeconds(int value, const std::string& unit, long long& outSeconds) {
    if (value <= 0) {
        return false;
    }

    if (unit == "minute") {
        outSeconds = static_cast<long long>(value) * 60LL;
        return true;
    }
    if (unit == "hour") {
        outSeconds = static_cast<long long>(value) * 3600LL;
        return true;
    }
    if (unit == "day") {
        outSeconds = static_cast<long long>(value) * 86400LL;
        return true;
    }
    if (unit == "week") {
        outSeconds = static_cast<long long>(value) * 604800LL;
        return true;
    }

    return false;
}

static long long addCalendarInterval(long long epochSeconds, int value, const std::string& unit) {
    std::time_t raw = static_cast<std::time_t>(epochSeconds);
    std::tm* local = std::localtime(&raw);
    if (!local) {
        return epochSeconds;
    }

    std::tm next = *local;
    if (unit == "month") {
        next.tm_mon += value;
    } else if (unit == "year") {
        next.tm_year += value;
    } else {
        return epochSeconds;
    }

    next.tm_isdst = -1;
    std::time_t converted = std::mktime(&next);
    if (converted == static_cast<std::time_t>(-1)) {
        return epochSeconds;
    }

    return static_cast<long long>(converted);
}

static int weekdayToIndex(const std::string& token) {
    const std::string day = lowerCopy(trimCopy(token));
    if (day == "sun" || day == "sunday") return 0;
    if (day == "mon" || day == "monday") return 1;
    if (day == "tue" || day == "tues" || day == "tuesday") return 2;
    if (day == "wed" || day == "wednesday") return 3;
    if (day == "thu" || day == "thur" || day == "thurs" || day == "thursday") return 4;
    if (day == "fri" || day == "friday") return 5;
    if (day == "sat" || day == "saturday") return 6;
    return -1;
}

static std::vector<int> parseSelectedDaysCsv(const std::string& csv) {
    std::vector<int> result;
    bool used[7] = {false, false, false, false, false, false, false};
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        const int day = weekdayToIndex(token);
        if (day >= 0 && day <= 6 && !used[day]) {
            used[day] = true;
            result.push_back(day);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

static long long nextSelectedDayOccurrence(long long dueEpochSeconds, const std::vector<int>& selectedDays) {
    if (selectedDays.empty()) {
        return dueEpochSeconds;
    }

    std::time_t raw = static_cast<std::time_t>(dueEpochSeconds);
    std::tm* local = std::localtime(&raw);
    if (!local) {
        return dueEpochSeconds;
    }

    std::tm base = *local;
    for (int offsetDays = 1; offsetDays <= 14; ++offsetDays) {
        std::tm candidate = base;
        candidate.tm_mday += offsetDays;
        candidate.tm_isdst = -1;

        std::time_t converted = std::mktime(&candidate);
        if (converted == static_cast<std::time_t>(-1)) {
            continue;
        }

        std::tm* check = std::localtime(&converted);
        if (!check) {
            continue;
        }

        const int weekday = check->tm_wday;
        if (std::find(selectedDays.begin(), selectedDays.end(), weekday) != selectedDays.end()) {
            return static_cast<long long>(converted);
        }
    }

    return dueEpochSeconds;
}

static long long advanceNextDueByRules(const DueReminder& reminder, long long dueEpochSeconds) {
    const std::string repeatMode = lowerCopy(reminder.repeatMode);
    if (repeatMode == "every_day") {
        return addCalendarInterval(dueEpochSeconds, 1, "day");
    }
    if (repeatMode == "every_x_days") {
        const int everyXDays = reminder.repeatEveryXDays > 0 ? reminder.repeatEveryXDays : 1;
        return addCalendarInterval(dueEpochSeconds, everyXDays, "day");
    }
    if (repeatMode == "selected_days") {
        return nextSelectedDayOccurrence(dueEpochSeconds, parseSelectedDaysCsv(reminder.selectedDays));
    }

    const int repeatValue = reminder.repeatValue;
    const std::string repeatUnit = lowerCopy(reminder.repeatUnit);
    if (repeatValue <= 0 || !isSupportedRepeatUnit(repeatUnit)) {
        return dueEpochSeconds;
    }

    return addCalendarInterval(dueEpochSeconds, repeatValue, repeatUnit);
}

static long long nextDueAfterNow(const DueReminder& reminder, long long nowEpochSeconds, int& missedCount) {
    missedCount = 0;
    long long next = reminder.nextDueAt;
    const int kMaxSteps = 500000;
    while (next <= nowEpochSeconds && missedCount < kMaxSteps) {
        long long advanced = advanceNextDueByRules(reminder, next);
        if (advanced <= next) {
            break;
        }
        next = advanced;
        ++missedCount;
    }
    return next;
}

static bool isDurationExpired(const DueReminder& reminder, long long dueEpochSeconds) {
    const std::string durationType = lowerCopy(reminder.durationType);
    if (durationType != "number_of_days") {
        return false;
    }
    if (reminder.endAt <= 0) {
        return false;
    }
    return dueEpochSeconds > reminder.endAt;
}

static const char* kPasswordHashPrefix = "pbkdf2_sha256";
static const unsigned long kPasswordHashIterations = 120000UL;
static const size_t kPasswordSaltBytes = 16U;
static const size_t kPasswordDerivedBytes = 32U;

static std::string bytesToHex(const unsigned char* data, size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<int>(data[i]);
    }
    return out.str();
}

static bool hexToBytes(const std::string& hex, std::vector<unsigned char>& out) {
    if (hex.empty() || (hex.size() % 2) != 0) {
        return false;
    }

    auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };

    out.assign(hex.size() / 2, 0);
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = hexValue(hex[i * 2]);
        const int lo = hexValue(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return true;
}

static bool constantTimeEquals(const std::vector<unsigned char>& a,
                               const std::vector<unsigned char>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

static bool splitPasswordHash(const std::string& stored,
                              std::string& prefix,
                              unsigned long& iterations,
                              std::string& saltHex,
                              std::string& digestHex) {
    std::array<std::string, 4> parts{};
    size_t partIndex = 0;
    size_t start = 0;
    while (partIndex < parts.size() - 1) {
        size_t sep = stored.find('$', start);
        if (sep == std::string::npos) {
            return false;
        }
        parts[partIndex++] = stored.substr(start, sep - start);
        start = sep + 1;
    }
    parts[partIndex++] = stored.substr(start);

    if (partIndex != 4) {
        return false;
    }

    prefix = parts[0];
    try {
        iterations = static_cast<unsigned long>(std::stoul(parts[1]));
    } catch (...) {
        return false;
    }
    saltHex = parts[2];
    digestHex = parts[3];
    return true;
}

static inline uint32_t rotr32(uint32_t value, uint32_t shift) {
    return (value >> shift) | (value << (32U - shift));
}

static void sha256Bytes(const std::vector<unsigned char>& input,
                        std::array<unsigned char, 32>& digest) {
    static const uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };

    uint32_t h0 = 0x6a09e667U;
    uint32_t h1 = 0xbb67ae85U;
    uint32_t h2 = 0x3c6ef372U;
    uint32_t h3 = 0xa54ff53aU;
    uint32_t h4 = 0x510e527fU;
    uint32_t h5 = 0x9b05688cU;
    uint32_t h6 = 0x1f83d9abU;
    uint32_t h7 = 0x5be0cd19U;

    std::vector<unsigned char> message = input;
    const uint64_t bitLength = static_cast<uint64_t>(message.size()) * 8ULL;

    message.push_back(0x80U);
    while ((message.size() % 64U) != 56U) {
        message.push_back(0x00U);
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<unsigned char>((bitLength >> (i * 8)) & 0xffU));
    }

    for (size_t chunk = 0; chunk < message.size(); chunk += 64U) {
        uint32_t w[64] = {0};
        for (int i = 0; i < 16; ++i) {
            const size_t idx = chunk + static_cast<size_t>(i) * 4U;
            w[i] = (static_cast<uint32_t>(message[idx]) << 24) |
                   (static_cast<uint32_t>(message[idx + 1]) << 16) |
                   (static_cast<uint32_t>(message[idx + 2]) << 8) |
                   (static_cast<uint32_t>(message[idx + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h0 += a; h1 += b; h2 += c; h3 += d;
        h4 += e; h5 += f; h6 += g; h7 += h;
    }

    const uint32_t outWords[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<unsigned char>((outWords[i] >> 24) & 0xffU);
        digest[i * 4 + 1] = static_cast<unsigned char>((outWords[i] >> 16) & 0xffU);
        digest[i * 4 + 2] = static_cast<unsigned char>((outWords[i] >> 8) & 0xffU);
        digest[i * 4 + 3] = static_cast<unsigned char>(outWords[i] & 0xffU);
    }
}

static void hmacSha256(const std::vector<unsigned char>& key,
                       const std::vector<unsigned char>& data,
                       std::array<unsigned char, 32>& out) {
    const size_t blockSize = 64U;
    std::vector<unsigned char> keyBlock = key;

    if (keyBlock.size() > blockSize) {
        std::array<unsigned char, 32> keyDigest{};
        sha256Bytes(keyBlock, keyDigest);
        keyBlock.assign(keyDigest.begin(), keyDigest.end());
    }
    keyBlock.resize(blockSize, 0x00U);

    std::vector<unsigned char> oKeyPad(blockSize, 0x5cU);
    std::vector<unsigned char> iKeyPad(blockSize, 0x36U);
    for (size_t i = 0; i < blockSize; ++i) {
        oKeyPad[i] ^= keyBlock[i];
        iKeyPad[i] ^= keyBlock[i];
    }

    std::vector<unsigned char> inner(iKeyPad.begin(), iKeyPad.end());
    inner.insert(inner.end(), data.begin(), data.end());
    std::array<unsigned char, 32> innerDigest{};
    sha256Bytes(inner, innerDigest);

    std::vector<unsigned char> outer(oKeyPad.begin(), oKeyPad.end());
    outer.insert(outer.end(), innerDigest.begin(), innerDigest.end());
    sha256Bytes(outer, out);
}

static bool generateRandomBytes(unsigned char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return false;
    }
    sqlite3_randomness(static_cast<int>(outSize), out);
    return true;
}

static bool derivePbkdf2Sha256(const std::string& password,
                               const std::vector<unsigned char>& salt,
                               unsigned long iterations,
                               std::vector<unsigned char>& out) {
    if (iterations == 0) {
        return false;
    }

    const std::vector<unsigned char> passwordBytes(password.begin(), password.end());
    std::vector<unsigned char> blockData = salt;
    blockData.push_back(0x00U);
    blockData.push_back(0x00U);
    blockData.push_back(0x00U);
    blockData.push_back(0x01U);

    std::array<unsigned char, 32> u{};
    hmacSha256(passwordBytes, blockData, u);
    std::array<unsigned char, 32> t = u;

    for (unsigned long i = 1; i < iterations; ++i) {
        std::vector<unsigned char> uInput(u.begin(), u.end());
        hmacSha256(passwordBytes, uInput, u);
        for (size_t j = 0; j < t.size(); ++j) {
            t[j] ^= u[j];
        }
    }

    out.assign(t.begin(), t.end());
    return true;
}

std::string Database::columnText(sqlite3_stmt* stmt, int columnIndex) {
    const unsigned char* text = sqlite3_column_text(stmt, columnIndex);
    return text ? reinterpret_cast<const char*>(text) : "";
}

bool Database::execute(const std::string& sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Database error: " << (errMsg ? errMsg : "Unknown error") << "\n";
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::hasColumn(const std::string& tableName, const std::string& columnName) {
    std::string pragmaSql = "PRAGMA table_info(" + tableName + ");";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, pragmaSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to inspect schema for " << tableName << ".\n";
        return false;
    }

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (columnText(stmt, 1) == columnName) {
            found = true;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return found;
}

bool Database::addColumnIfMissing(const std::string& tableName,
                                  const std::string& columnName,
                                  const std::string& columnDefinition) {
    if (hasColumn(tableName, columnName)) {
        return true;
    }

    return execute("ALTER TABLE " + tableName + " ADD COLUMN " + columnDefinition + ";");
}

Database::Database(const std::string& fileName) {
    if (sqlite3_open(fileName.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Failed to open database: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        db = nullptr;
        return;
    }
    execute("PRAGMA foreign_keys = ON;");
    initialize();
}

Database::~Database() {
    if (db) {
        sqlite3_close(db);
    }
}

bool Database::isOpen() const { return db != nullptr; }

bool Database::initialize() {
    if (!db) {
        return false;
    }

    const std::string createAccounts =
        "CREATE TABLE IF NOT EXISTS accounts ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "username TEXT UNIQUE NOT NULL, "
        "password TEXT NOT NULL"
        ");";

    const std::string createUsers =
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "account_id INTEGER UNIQUE NOT NULL, "
        "name TEXT NOT NULL, "
        "age INTEGER NOT NULL, "
        "gender TEXT NOT NULL, "
        "medical_issue TEXT, "
        "contact_number TEXT NOT NULL, "
        "emergency_contact TEXT, "
        "profile_photo_path TEXT NOT NULL DEFAULT '', "
        "FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE"
        ");";

    const std::string createMedicines =
        "CREATE TABLE IF NOT EXISTS medicines ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "account_id INTEGER NOT NULL, "
        "name TEXT NOT NULL, "
        "dosage TEXT NOT NULL, "
        "time TEXT NOT NULL, "
        "taken INTEGER NOT NULL DEFAULT 0 CHECK(taken IN (0,1)), "
        "next_due_at INTEGER NOT NULL DEFAULT 0 CHECK(next_due_at >= 0), "
        "repeat_minutes INTEGER NOT NULL DEFAULT 0 CHECK(repeat_minutes >= 0), "
        "repeat_value INTEGER NOT NULL DEFAULT 0 CHECK(repeat_value >= 0), "
        "repeat_unit TEXT NOT NULL DEFAULT '', "
        "enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)), "
        "enable_reminder INTEGER NOT NULL DEFAULT 1 CHECK(enable_reminder IN (0,1)), "
        "frequency TEXT NOT NULL DEFAULT 'specific_times' CHECK(frequency = 'specific_times'), "
        "repeat_mode TEXT NOT NULL DEFAULT 'every_day' CHECK(repeat_mode IN ('every_day','selected_days','every_x_days')), "
        "repeat_every_x_days INTEGER NOT NULL DEFAULT 1 CHECK(repeat_every_x_days > 0), "
        "selected_days TEXT NOT NULL DEFAULT '', "
        "start_date TEXT NOT NULL DEFAULT '', "
        "duration_type TEXT NOT NULL DEFAULT 'continuous' CHECK(duration_type IN ('continuous','number_of_days')), "
        "duration_days INTEGER NOT NULL DEFAULT 0 CHECK(duration_days >= 0), "
        "end_at INTEGER NOT NULL DEFAULT 0 CHECK(end_at >= 0), "
        "FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE"
        ");";

    bool ok = execute(createAccounts) && execute(createUsers) && execute(createMedicines);
    if (!ok) {
        return false;
    }

    ok = addColumnIfMissing("users", "profile_photo_path", "profile_photo_path TEXT NOT NULL DEFAULT ''") &&
         addColumnIfMissing("medicines", "next_due_at", "next_due_at INTEGER NOT NULL DEFAULT 0") &&
         addColumnIfMissing("medicines", "repeat_minutes", "repeat_minutes INTEGER NOT NULL DEFAULT 0") &&
         addColumnIfMissing("medicines", "repeat_value", "repeat_value INTEGER NOT NULL DEFAULT 0") &&
         addColumnIfMissing("medicines", "repeat_unit", "repeat_unit TEXT NOT NULL DEFAULT ''") &&
         addColumnIfMissing("medicines", "enabled", "enabled INTEGER NOT NULL DEFAULT 1") &&
         addColumnIfMissing("medicines", "enable_reminder", "enable_reminder INTEGER NOT NULL DEFAULT 1") &&
         addColumnIfMissing("medicines", "frequency", "frequency TEXT NOT NULL DEFAULT 'specific_times'") &&
         addColumnIfMissing("medicines", "repeat_mode", "repeat_mode TEXT NOT NULL DEFAULT 'every_day'") &&
         addColumnIfMissing("medicines", "repeat_every_x_days", "repeat_every_x_days INTEGER NOT NULL DEFAULT 1") &&
         addColumnIfMissing("medicines", "selected_days", "selected_days TEXT NOT NULL DEFAULT ''") &&
         addColumnIfMissing("medicines", "start_date", "start_date TEXT NOT NULL DEFAULT ''") &&
         addColumnIfMissing("medicines", "duration_type", "duration_type TEXT NOT NULL DEFAULT 'continuous'") &&
         addColumnIfMissing("medicines", "duration_days", "duration_days INTEGER NOT NULL DEFAULT 0") &&
         addColumnIfMissing("medicines", "end_at", "end_at INTEGER NOT NULL DEFAULT 0");

    if (!ok) {
        return false;
    }

    const std::string createMissedReminders =
        "CREATE TABLE IF NOT EXISTS missed_reminder_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "account_id INTEGER NOT NULL, "
        "medicine_id INTEGER NOT NULL, "
        "medicine_name TEXT NOT NULL, "
        "missed_count INTEGER NOT NULL CHECK(missed_count > 0), "
        "recorded_at INTEGER NOT NULL CHECK(recorded_at >= 0), "
        "next_due_at INTEGER NOT NULL CHECK(next_due_at >= 0), "
        "FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE CASCADE, "
        "FOREIGN KEY (medicine_id) REFERENCES medicines(id) ON DELETE CASCADE"
        ");";

    const std::string createAuditLogs =
        "CREATE TABLE IF NOT EXISTS audit_logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "account_id INTEGER, "
        "event_type TEXT NOT NULL, "
        "details TEXT NOT NULL, "
        "created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)), "
        "FOREIGN KEY (account_id) REFERENCES accounts(id) ON DELETE SET NULL"
        ");";

    const std::string indexDueLookup =
        "CREATE INDEX IF NOT EXISTS idx_medicines_account_enabled_due "
        "ON medicines(account_id, enabled, next_due_at);";
    const std::string indexDueGlobal =
        "CREATE INDEX IF NOT EXISTS idx_medicines_enabled_due "
        "ON medicines(enabled, next_due_at);";
    const std::string indexMissedByAccount =
        "CREATE INDEX IF NOT EXISTS idx_missed_events_account_recorded "
        "ON missed_reminder_events(account_id, recorded_at DESC);";
    const std::string indexAuditByAccount =
        "CREATE INDEX IF NOT EXISTS idx_audit_logs_account_created "
        "ON audit_logs(account_id, created_at DESC);";

    if (!execute(createMissedReminders) ||
        !execute(createAuditLogs) ||
        !execute(indexDueLookup) ||
        !execute(indexDueGlobal) ||
        !execute(indexMissedByAccount) ||
        !execute(indexAuditByAccount)) {
        return false;
    }

    // Backfill old rows created before scheduling fields existed.
    const std::string normalizeRepeat =
        "UPDATE medicines SET repeat_minutes = 0 WHERE repeat_minutes IS NULL;";
    const std::string normalizeRepeatValue =
        "UPDATE medicines SET repeat_value = repeat_minutes "
        "WHERE (repeat_value IS NULL OR repeat_value <= 0) AND repeat_minutes > 0;";
    const std::string normalizeRepeatUnit =
        "UPDATE medicines SET repeat_unit = 'minute' "
        "WHERE (repeat_unit IS NULL OR repeat_unit = '') AND repeat_value > 0;";
    const std::string normalizeDue =
        "UPDATE medicines "
        "SET next_due_at = CAST(strftime('%s','now') AS INTEGER) "
        "WHERE next_due_at IS NULL OR next_due_at <= 0;";
    const std::string normalizeEnabled =
        "UPDATE medicines SET enabled = 1 WHERE enabled IS NULL;";
    const std::string normalizeEnableReminder =
        "UPDATE medicines SET enable_reminder = enabled WHERE enable_reminder IS NULL;";
    const std::string normalizeFrequency =
        "UPDATE medicines SET frequency = 'specific_times' "
        "WHERE frequency IS NULL OR frequency = '';";
    const std::string normalizeRepeatMode =
        "UPDATE medicines SET repeat_mode = "
        "CASE "
        "WHEN repeat_unit = 'day' AND repeat_value = 1 THEN 'every_day' "
        "WHEN repeat_unit = 'day' AND repeat_value > 1 THEN 'every_x_days' "
        "WHEN repeat_unit = 'week' THEN 'selected_days' "
        "ELSE 'every_day' "
        "END "
        "WHERE repeat_mode IS NULL OR repeat_mode = '';";
    const std::string normalizeEveryXDays =
        "UPDATE medicines SET repeat_every_x_days = "
        "CASE "
        "WHEN repeat_mode = 'every_x_days' AND repeat_value > 0 THEN repeat_value "
        "ELSE 1 "
        "END "
        "WHERE repeat_every_x_days IS NULL OR repeat_every_x_days <= 0;";
    const std::string normalizeSelectedDays =
        "UPDATE medicines SET selected_days = "
        "CASE strftime('%w', next_due_at, 'unixepoch', 'localtime') "
        "WHEN '0' THEN 'sun' "
        "WHEN '1' THEN 'mon' "
        "WHEN '2' THEN 'tue' "
        "WHEN '3' THEN 'wed' "
        "WHEN '4' THEN 'thu' "
        "WHEN '5' THEN 'fri' "
        "WHEN '6' THEN 'sat' "
        "ELSE 'mon' "
        "END "
        "WHERE repeat_mode = 'selected_days' AND (selected_days IS NULL OR selected_days = '');";
    const std::string normalizeStartDate =
        "UPDATE medicines SET start_date = strftime('%Y-%m-%d', next_due_at, 'unixepoch', 'localtime') "
        "WHERE start_date IS NULL OR start_date = '';";
    const std::string normalizeDurationType =
        "UPDATE medicines SET duration_type = 'continuous' "
        "WHERE duration_type IS NULL OR duration_type = '';";
    const std::string normalizeDurationDays =
        "UPDATE medicines SET duration_days = 0 WHERE duration_days IS NULL OR duration_days < 0;";
    const std::string normalizeEndAt =
        "UPDATE medicines SET end_at = 0 WHERE end_at IS NULL OR end_at < 0;";
    const std::string syncEnabledColumns =
        "UPDATE medicines SET enabled = CASE WHEN enable_reminder = 1 THEN 1 ELSE 0 END;";
    const std::string deleteDuplicateMedicines =
        "DELETE FROM medicines "
        "WHERE id NOT IN ("
        "SELECT MIN(id) FROM medicines "
        "GROUP BY account_id, name, dosage, time, start_date, repeat_mode, repeat_every_x_days, selected_days, duration_type, duration_days, enable_reminder"
        ");";
    const std::string uniqueMedicineRule =
        "CREATE UNIQUE INDEX IF NOT EXISTS ux_medicines_account_schedule "
        "ON medicines(account_id, name, dosage, time, start_date, repeat_mode, repeat_every_x_days, selected_days, duration_type, duration_days, enable_reminder);";
    const std::string triggerCheckInsert =
        "CREATE TRIGGER IF NOT EXISTS trg_medicines_check_insert "
        "BEFORE INSERT ON medicines "
        "BEGIN "
        "SELECT CASE WHEN NEW.repeat_mode NOT IN ('every_day','selected_days','every_x_days') "
        "THEN RAISE(ABORT, 'invalid repeat_mode') END; "
        "SELECT CASE WHEN NEW.duration_type NOT IN ('continuous','number_of_days') "
        "THEN RAISE(ABORT, 'invalid duration_type') END; "
        "SELECT CASE WHEN NEW.enable_reminder NOT IN (0,1) OR NEW.enabled NOT IN (0,1) OR NEW.taken NOT IN (0,1) "
        "THEN RAISE(ABORT, 'invalid flag value') END; "
        "SELECT CASE WHEN NEW.repeat_every_x_days <= 0 OR NEW.repeat_value < 0 OR NEW.repeat_minutes < 0 "
        "THEN RAISE(ABORT, 'invalid repeat value') END; "
        "SELECT CASE WHEN NEW.duration_type = 'number_of_days' AND NEW.duration_days <= 0 "
        "THEN RAISE(ABORT, 'invalid duration_days') END; "
        "SELECT CASE WHEN NEW.duration_type = 'continuous' AND NEW.duration_days <> 0 "
        "THEN RAISE(ABORT, 'continuous requires duration_days=0') END; "
        "SELECT CASE WHEN NEW.repeat_mode = 'selected_days' AND (NEW.selected_days IS NULL OR NEW.selected_days = '') "
        "THEN RAISE(ABORT, 'selected_days required') END; "
        "END;";
    const std::string triggerCheckUpdate =
        "CREATE TRIGGER IF NOT EXISTS trg_medicines_check_update "
        "BEFORE UPDATE ON medicines "
        "BEGIN "
        "SELECT CASE WHEN NEW.repeat_mode NOT IN ('every_day','selected_days','every_x_days') "
        "THEN RAISE(ABORT, 'invalid repeat_mode') END; "
        "SELECT CASE WHEN NEW.duration_type NOT IN ('continuous','number_of_days') "
        "THEN RAISE(ABORT, 'invalid duration_type') END; "
        "SELECT CASE WHEN NEW.enable_reminder NOT IN (0,1) OR NEW.enabled NOT IN (0,1) OR NEW.taken NOT IN (0,1) "
        "THEN RAISE(ABORT, 'invalid flag value') END; "
        "SELECT CASE WHEN NEW.repeat_every_x_days <= 0 OR NEW.repeat_value < 0 OR NEW.repeat_minutes < 0 "
        "THEN RAISE(ABORT, 'invalid repeat value') END; "
        "SELECT CASE WHEN NEW.duration_type = 'number_of_days' AND NEW.duration_days <= 0 "
        "THEN RAISE(ABORT, 'invalid duration_days') END; "
        "SELECT CASE WHEN NEW.duration_type = 'continuous' AND NEW.duration_days <> 0 "
        "THEN RAISE(ABORT, 'continuous requires duration_days=0') END; "
        "SELECT CASE WHEN NEW.repeat_mode = 'selected_days' AND (NEW.selected_days IS NULL OR NEW.selected_days = '') "
        "THEN RAISE(ABORT, 'selected_days required') END; "
        "END;";
    const bool schemaOk =
        execute(normalizeRepeat) &&
        execute(normalizeRepeatValue) &&
        execute(normalizeRepeatUnit) &&
        execute(normalizeDue) &&
        execute(normalizeEnabled) &&
        execute(normalizeEnableReminder) &&
        execute(normalizeFrequency) &&
        execute(normalizeRepeatMode) &&
        execute(normalizeEveryXDays) &&
        execute(normalizeSelectedDays) &&
        execute(normalizeStartDate) &&
        execute(normalizeDurationType) &&
        execute(normalizeDurationDays) &&
        execute(normalizeEndAt) &&
        execute(syncEnabledColumns) &&
        execute(deleteDuplicateMedicines) &&
        execute(uniqueMedicineRule) &&
        execute(triggerCheckInsert) &&
        execute(triggerCheckUpdate);
    if (!schemaOk) {
        return false;
    }

    // Upgrade legacy plaintext passwords to hashed form.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, password FROM accounts;", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    std::vector<std::pair<int, std::string>> legacyPasswords;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int accountId = sqlite3_column_int(stmt, 0);
        const std::string storedPassword = columnText(stmt, 1);

        std::string prefix;
        unsigned long iterations = 0;
        std::string saltHex;
        std::string digestHex;
        if (!splitPasswordHash(storedPassword, prefix, iterations, saltHex, digestHex) ||
            prefix != kPasswordHashPrefix) {
            legacyPasswords.push_back({accountId, storedPassword});
        }
    }
    sqlite3_finalize(stmt);

    for (const auto& legacy : legacyPasswords) {
        std::string hashed;
        if (!hashPassword(legacy.second, hashed)) {
            return false;
        }
        if (!updateStoredPasswordHash(legacy.first, hashed)) {
            return false;
        }
        insertAuditLog(legacy.first, "account.password_migrated", "Legacy plaintext password migrated");
    }

    return true;
}

bool Database::hashPassword(const std::string& plainPassword, std::string& outHash) {
    std::vector<unsigned char> salt(kPasswordSaltBytes, 0);
    if (!generateRandomBytes(salt.data(), salt.size())) {
        return false;
    }

    std::vector<unsigned char> derived;
    if (!derivePbkdf2Sha256(plainPassword, salt, kPasswordHashIterations, derived)) {
        return false;
    }

    outHash = std::string(kPasswordHashPrefix) + "$" +
              std::to_string(kPasswordHashIterations) + "$" +
              bytesToHex(salt.data(), salt.size()) + "$" +
              bytesToHex(derived.data(), derived.size());
    return true;
}

bool Database::verifyPasswordValue(const std::string& plainPassword,
                                   const std::string& storedValue,
                                   bool& isValid,
                                   bool& needsUpgrade) {
    isValid = false;
    needsUpgrade = false;

    if (storedValue.empty()) {
        return true;
    }

    std::string prefix;
    unsigned long iterations = 0;
    std::string saltHex;
    std::string digestHex;

    if (splitPasswordHash(storedValue, prefix, iterations, saltHex, digestHex)) {
        if (prefix != kPasswordHashPrefix || iterations == 0) {
            return true;
        }

        std::vector<unsigned char> salt;
        std::vector<unsigned char> expectedDigest;
        if (!hexToBytes(saltHex, salt) || !hexToBytes(digestHex, expectedDigest) || salt.empty() || expectedDigest.empty()) {
            return true;
        }

        std::vector<unsigned char> derived;
        if (!derivePbkdf2Sha256(plainPassword, salt, iterations, derived)) {
            return false;
        }

        isValid = constantTimeEquals(derived, expectedDigest);
        needsUpgrade = isValid;
        return true;
    }

    // Legacy plaintext row: verify once, then migrate to hash.
    isValid = (plainPassword == storedValue);
    needsUpgrade = isValid;
    return true;
}

bool Database::updateStoredPasswordHash(int accountId, const std::string& passwordHash) {
    const char* sql = "UPDATE accounts SET password = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, passwordHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, accountId);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) > 0;
}

void Database::insertAuditLog(int accountId, const std::string& eventType, const std::string& details) {
    const char* sql =
        "INSERT INTO audit_logs(account_id, event_type, details, created_at) "
        "VALUES(?, ?, ?, CAST(strftime('%s','now') AS INTEGER));";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    if (accountId > 0) {
        sqlite3_bind_int(stmt, 1, accountId);
    } else {
        sqlite3_bind_null(stmt, 1);
    }
    sqlite3_bind_text(stmt, 2, eventType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, details.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Database::insertMissedReminderEvent(const DueReminder& reminder,
                                         int missedCount,
                                         long long recordedAtEpochSeconds,
                                         long long nextDueAtEpochSeconds) {
    const char* sql =
        "INSERT INTO missed_reminder_events("
        "account_id, medicine_id, medicine_name, missed_count, recorded_at, next_due_at"
        ") VALUES(?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_int(stmt, 1, reminder.accountId);
    sqlite3_bind_int(stmt, 2, reminder.medicineId);
    sqlite3_bind_text(stmt, 3, reminder.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, missedCount);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(recordedAtEpochSeconds));
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(nextDueAtEpochSeconds));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool Database::createAccount(const std::string& username, const std::string& password, int& accountId) {
    const char* sql = "INSERT INTO accounts(username, password) VALUES(?, ?);";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare create account query.\n";
        return false;
    }

    std::string passwordHash;
    if (!hashPassword(password, passwordHash)) {
        std::cerr << "Could not hash password for account creation.\n";
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, passwordHash.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not create account: " << sqlite3_errmsg(db) << "\n";
        return false;
    }

    accountId = static_cast<int>(sqlite3_last_insert_rowid(db));
    insertAuditLog(accountId, "account.create", "Account created");
    return true;
}

bool Database::login(const std::string& username, const std::string& password, int& accountId) {
    const char* sql = "SELECT id, password FROM accounts WHERE username = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare login query.\n";
        return false;
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const int foundAccountId = sqlite3_column_int(stmt, 0);
        const std::string storedPassword = columnText(stmt, 1);

        bool isValid = false;
        bool needsUpgrade = false;
        if (verifyPasswordValue(password, storedPassword, isValid, needsUpgrade) && isValid) {
            accountId = foundAccountId;
            ok = true;

            if (needsUpgrade) {
                std::string upgradedHash;
                if (hashPassword(password, upgradedHash)) {
                    updateStoredPasswordHash(accountId, upgradedHash);
                }
            }
        }
    }

    sqlite3_finalize(stmt);
    if (ok) {
        insertAuditLog(accountId, "account.login", "Login successful");
    }
    return ok;
}

bool Database::getAccountById(int accountId, std::string& username, std::string& password) {
    const char* sql = "SELECT username, password FROM accounts WHERE id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare get account query.\n";
        return false;
    }

    sqlite3_bind_int(stmt, 1, accountId);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        username = columnText(stmt, 0);
        password = columnText(stmt, 1);
        found = true;
    }

    sqlite3_finalize(stmt);
    return found;
}

bool Database::verifyAccountPassword(int accountId, const std::string& password) {
    const char* sql = "SELECT password FROM accounts WHERE id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare verify password query.\n";
        return false;
    }

    sqlite3_bind_int(stmt, 1, accountId);

    bool isValid = false;
    bool needsUpgrade = false;
    bool matched = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string storedPassword = columnText(stmt, 0);
        if (verifyPasswordValue(password, storedPassword, isValid, needsUpgrade) && isValid) {
            matched = true;
        }
    }
    sqlite3_finalize(stmt);

    if (matched && needsUpgrade) {
        std::string upgradedHash;
        if (hashPassword(password, upgradedHash)) {
            updateStoredPasswordHash(accountId, upgradedHash);
        }
    }
    return matched;
}

bool Database::updateUsername(int accountId, const std::string& newUsername) {
    const char* sql = "UPDATE accounts SET username = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare username update query.\n";
        return false;
    }

    sqlite3_bind_text(stmt, 1, newUsername.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, accountId);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not update username: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    const bool changed = sqlite3_changes(db) > 0;
    if (changed) {
        insertAuditLog(accountId, "account.update_username", "Username updated");
    }
    return changed;
}

bool Database::updatePassword(int accountId, const std::string& newPassword) {
    const char* sql = "UPDATE accounts SET password = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare password update query.\n";
        return false;
    }

    std::string passwordHash;
    if (!hashPassword(newPassword, passwordHash)) {
        std::cerr << "Could not hash password for update.\n";
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_bind_text(stmt, 1, passwordHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, accountId);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not update password: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    const bool changed = sqlite3_changes(db) > 0;
    if (changed) {
        insertAuditLog(accountId, "account.update_password", "Password updated");
    }
    return changed;
}

bool Database::saveUser(int accountId, const User& user) {
    const char* updateSql =
        "UPDATE users SET name = ?, age = ?, gender = ?, medical_issue = ?, "
        "contact_number = ?, emergency_contact = ? WHERE account_id = ?;";

    sqlite3_stmt* updateStmt = nullptr;
    if (sqlite3_prepare_v2(db, updateSql, -1, &updateStmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare user update query.\n";
        return false;
    }

    sqlite3_bind_text(updateStmt, 1, user.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(updateStmt, 2, user.age);
    sqlite3_bind_text(updateStmt, 3, user.gender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(updateStmt, 4, user.medicalIssue.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(updateStmt, 5, user.contactNumber.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(updateStmt, 6, user.emergencyContact.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(updateStmt, 7, accountId);

    int rc = sqlite3_step(updateStmt);
    sqlite3_finalize(updateStmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not update user: " << sqlite3_errmsg(db) << "\n";
        return false;
    }

    if (sqlite3_changes(db) > 0) {
        return true;
    }

    const char* insertSql =
        "INSERT INTO users(account_id, name, age, gender, medical_issue, contact_number, emergency_contact) "
        "VALUES(?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* insertStmt = nullptr;
    if (sqlite3_prepare_v2(db, insertSql, -1, &insertStmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare user insert query.\n";
        return false;
    }

    sqlite3_bind_int(insertStmt, 1, accountId);
    sqlite3_bind_text(insertStmt, 2, user.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insertStmt, 3, user.age);
    sqlite3_bind_text(insertStmt, 4, user.gender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 5, user.medicalIssue.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 6, user.contactNumber.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 7, user.emergencyContact.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(insertStmt);
    sqlite3_finalize(insertStmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not insert user: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    return true;
}

bool Database::loadUser(int accountId, User& user) {
    const char* sql =
        "SELECT name, age, gender, medical_issue, contact_number, emergency_contact, profile_photo_path "
        "FROM users WHERE account_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare load user query.\n";
        return false;
    }

    sqlite3_bind_int(stmt, 1, accountId);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        user.name = columnText(stmt, 0);
        user.age = sqlite3_column_int(stmt, 1);
        user.gender = columnText(stmt, 2);
        user.medicalIssue = columnText(stmt, 3);
        user.contactNumber = columnText(stmt, 4);
        user.emergencyContact = columnText(stmt, 5);
        user.profilePhotoPath = columnText(stmt, 6);
        found = true;
    }

    sqlite3_finalize(stmt);
    return found;
}

bool Database::updateUserProfilePhotoPath(int accountId, const std::string& photoPath) {
    const char* updateSql = "UPDATE users SET profile_photo_path = ? WHERE account_id = ?;";
    sqlite3_stmt* updateStmt = nullptr;
    if (sqlite3_prepare_v2(db, updateSql, -1, &updateStmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare profile photo update query.\n";
        return false;
    }

    sqlite3_bind_text(updateStmt, 1, photoPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(updateStmt, 2, accountId);
    int rc = sqlite3_step(updateStmt);
    sqlite3_finalize(updateStmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not update profile photo: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    if (sqlite3_changes(db) > 0) {
        return true;
    }

    const char* insertSql =
        "INSERT INTO users(account_id, name, age, gender, medical_issue, contact_number, emergency_contact, profile_photo_path) "
        "VALUES(?, '', 0, '', '', '', '', ?);";
    sqlite3_stmt* insertStmt = nullptr;
    if (sqlite3_prepare_v2(db, insertSql, -1, &insertStmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare profile photo insert query.\n";
        return false;
    }

    sqlite3_bind_int(insertStmt, 1, accountId);
    sqlite3_bind_text(insertStmt, 2, photoPath.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(insertStmt);
    sqlite3_finalize(insertStmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not insert profile photo record: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    return true;
}

bool Database::getUserProfilePhotoPath(int accountId, std::string& photoPath) {
    const char* sql = "SELECT profile_photo_path FROM users WHERE account_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare get profile photo query.\n";
        return false;
    }

    sqlite3_bind_int(stmt, 1, accountId);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        photoPath = columnText(stmt, 0);
        found = true;
    } else {
        photoPath.clear();
    }
    sqlite3_finalize(stmt);
    return found;
}

int Database::addMedicine(int accountId, const Medicine& medicine) {
    const char* sql =
        "INSERT INTO medicines("
        "account_id, name, dosage, time, taken, next_due_at, repeat_minutes, repeat_value, repeat_unit, "
        "enabled, enable_reminder, frequency, repeat_mode, repeat_every_x_days, selected_days, "
        "start_date, duration_type, duration_days, end_at"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare add medicine query.\n";
        return -1;
    }

    long long nextDueAt = medicine.getNextDueAt();
    if (nextDueAt <= 0) {
        nextDueAt = static_cast<long long>(std::time(nullptr));
    }
    const bool enableReminder = medicine.isEnabled();

    std::string frequency = lowerCopy(trimCopy(medicine.getFrequency()));
    if (frequency.empty()) {
        frequency = "specific_times";
    }
    if (frequency != "specific_times") {
        std::cerr << "Could not add medicine: invalid frequency.\n";
        sqlite3_finalize(stmt);
        return -1;
    }

    std::string repeatMode = lowerCopy(trimCopy(medicine.getRepeatMode()));
    if (repeatMode.empty()) {
        const int legacyRepeatValue = medicine.getRepeatValue();
        const std::string legacyRepeatUnit = lowerCopy(trimCopy(medicine.getRepeatUnit()));
        if (legacyRepeatUnit == "day" && legacyRepeatValue > 1) {
            repeatMode = "every_x_days";
        } else if (legacyRepeatUnit == "week") {
            repeatMode = "selected_days";
        } else {
            repeatMode = "every_day";
        }
    }
    if (!isSupportedRepeatMode(repeatMode)) {
        std::cerr << "Could not add medicine: invalid repeat mode.\n";
        sqlite3_finalize(stmt);
        return -1;
    }

    int repeatEveryXDays = medicine.getRepeatEveryXDays();
    if (repeatEveryXDays <= 0) {
        repeatEveryXDays = medicine.getRepeatValue() > 0 ? medicine.getRepeatValue() : 1;
    }

    std::string selectedDays = lowerCopy(trimCopy(medicine.getSelectedDays()));
    if (repeatMode == "selected_days") {
        if (selectedDays.empty()) {
            selectedDays = "mon";
        }
        if (parseSelectedDaysCsv(selectedDays).empty()) {
            std::cerr << "Could not add medicine: invalid selected days.\n";
            sqlite3_finalize(stmt);
            return -1;
        }
    } else {
        selectedDays.clear();
    }

    std::string startDate = trimCopy(medicine.getStartDate());
    std::string durationType = lowerCopy(trimCopy(medicine.getDurationType()));
    if (durationType.empty()) {
        durationType = "continuous";
    }
    if (!isSupportedDurationType(durationType)) {
        std::cerr << "Could not add medicine: invalid duration type.\n";
        sqlite3_finalize(stmt);
        return -1;
    }

    int durationDays = medicine.getDurationDays();
    if (durationType == "continuous") {
        durationDays = 0;
    } else if (durationDays <= 0) {
        std::cerr << "Could not add medicine: invalid duration days.\n";
        sqlite3_finalize(stmt);
        return -1;
    }
    long long endAt = medicine.getEndAt();
    if (durationType == "continuous") {
        endAt = 0;
    }

    int repeatValue = 1;
    std::string repeatUnit = "day";
    int repeatMinutes = 1440;
    if (repeatMode == "every_x_days") {
        repeatValue = repeatEveryXDays;
        repeatUnit = "day";
        long long minutes64 = static_cast<long long>(repeatEveryXDays) * 1440LL;
        if (minutes64 > static_cast<long long>(std::numeric_limits<int>::max())) {
            std::cerr << "Could not add medicine: repeat interval too large.\n";
            sqlite3_finalize(stmt);
            return -1;
        }
        repeatMinutes = static_cast<int>(minutes64);
    } else if (repeatMode == "selected_days") {
        repeatValue = 1;
        repeatUnit = "week";
        repeatMinutes = 10080;
    }

    sqlite3_bind_int(stmt, 1, accountId);
    sqlite3_bind_text(stmt, 2, medicine.getName().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, medicine.getDosage().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, medicine.getTime().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, medicine.isTaken() ? 1 : 0);
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(nextDueAt));
    sqlite3_bind_int(stmt, 7, repeatMinutes);
    sqlite3_bind_int(stmt, 8, repeatValue);
    sqlite3_bind_text(stmt, 9, repeatUnit.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 10, enableReminder ? 1 : 0);
    sqlite3_bind_int(stmt, 11, enableReminder ? 1 : 0);
    sqlite3_bind_text(stmt, 12, frequency.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, repeatMode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 14, repeatEveryXDays);
    sqlite3_bind_text(stmt, 15, selectedDays.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 16, startDate.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 17, durationType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 18, durationDays);
    sqlite3_bind_int64(stmt, 19, static_cast<sqlite3_int64>(endAt));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not add medicine: " << sqlite3_errmsg(db) << "\n";
        return -1;
    }
    const int medicineId = static_cast<int>(sqlite3_last_insert_rowid(db));
    insertAuditLog(accountId, "medicine.add", "Added medicine: " + medicine.getName());
    return medicineId;
}

std::vector<Medicine> Database::loadMedicines(int accountId) {
    std::vector<Medicine> loaded;
    const char* sql =
        "SELECT id, name, dosage, time, taken, next_due_at, repeat_minutes, repeat_value, repeat_unit, enabled, "
        "frequency, repeat_mode, repeat_every_x_days, selected_days, start_date, duration_type, duration_days, end_at "
        "FROM medicines WHERE account_id = ? ORDER BY id;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare load medicines query.\n";
        return loaded;
    }

    sqlite3_bind_int(stmt, 1, accountId);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        std::string name = columnText(stmt, 1);
        std::string dosage = columnText(stmt, 2);
        std::string time = columnText(stmt, 3);
        bool taken = sqlite3_column_int(stmt, 4) == 1;
        long long nextDueAt = static_cast<long long>(sqlite3_column_int64(stmt, 5));
        int repeatMinutes = sqlite3_column_int(stmt, 6);
        int repeatValue = sqlite3_column_int(stmt, 7);
        std::string repeatUnit = columnText(stmt, 8);
        bool enabled = sqlite3_column_int(stmt, 9) == 1;
        std::string frequency = lowerCopy(columnText(stmt, 10));
        std::string repeatMode = lowerCopy(columnText(stmt, 11));
        int repeatEveryXDays = sqlite3_column_int(stmt, 12);
        std::string selectedDays = lowerCopy(columnText(stmt, 13));
        std::string startDate = columnText(stmt, 14);
        std::string durationType = lowerCopy(columnText(stmt, 15));
        int durationDays = sqlite3_column_int(stmt, 16);
        long long endAt = static_cast<long long>(sqlite3_column_int64(stmt, 17));

        loaded.emplace_back(name,
                            dosage,
                            time,
                            taken,
                            id,
                            nextDueAt,
                            repeatMinutes,
                            repeatValue,
                            repeatUnit,
                            enabled,
                            frequency,
                            repeatMode,
                            repeatEveryXDays,
                            selectedDays,
                            startDate,
                            durationType,
                            durationDays,
                            endAt);
    }

    sqlite3_finalize(stmt);
    return loaded;
}

bool Database::markMedicineTaken(int medicineId) {
    const char* sql = "UPDATE medicines SET taken = 1 WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare mark taken query.\n";
        return false;
    }

    sqlite3_bind_int(stmt, 1, medicineId);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not update medicine status: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    return sqlite3_changes(db) > 0;
}

bool Database::deleteMedicine(int medicineId) {
    const char* sql = "DELETE FROM medicines WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare delete medicine query.\n";
        return false;
    }

    sqlite3_bind_int(stmt, 1, medicineId);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not delete medicine: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    return sqlite3_changes(db) > 0;
}

std::vector<DueReminder> Database::loadDueReminders(long long nowEpochSeconds) {
    std::vector<DueReminder> reminders;
    const char* sql =
        "SELECT id, account_id, name, dosage, time, next_due_at, repeat_minutes, repeat_value, repeat_unit, "
        "enable_reminder, frequency, repeat_mode, repeat_every_x_days, selected_days, "
        "start_date, duration_type, duration_days, end_at "
        "FROM medicines "
        "WHERE enabled = 1 AND enable_reminder = 1 AND next_due_at > 0 AND next_due_at <= ? "
        "ORDER BY next_due_at ASC;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare due reminders query.\n";
        return reminders;
    }

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(nowEpochSeconds));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DueReminder reminder;
        reminder.medicineId = sqlite3_column_int(stmt, 0);
        reminder.accountId = sqlite3_column_int(stmt, 1);
        reminder.name = columnText(stmt, 2);
        reminder.dosage = columnText(stmt, 3);
        reminder.timeLabel = columnText(stmt, 4);
        reminder.nextDueAt = static_cast<long long>(sqlite3_column_int64(stmt, 5));
        reminder.repeatMinutes = sqlite3_column_int(stmt, 6);
        reminder.repeatValue = sqlite3_column_int(stmt, 7);
        reminder.repeatUnit = lowerCopy(columnText(stmt, 8));
        reminder.enabled = sqlite3_column_int(stmt, 9) == 1;
        reminder.frequency = lowerCopy(columnText(stmt, 10));
        reminder.repeatMode = lowerCopy(columnText(stmt, 11));
        reminder.repeatEveryXDays = sqlite3_column_int(stmt, 12);
        reminder.selectedDays = lowerCopy(columnText(stmt, 13));
        reminder.startDate = columnText(stmt, 14);
        reminder.durationType = lowerCopy(columnText(stmt, 15));
        reminder.durationDays = sqlite3_column_int(stmt, 16);
        reminder.endAt = static_cast<long long>(sqlite3_column_int64(stmt, 17));
        reminders.push_back(reminder);
    }

    sqlite3_finalize(stmt);
    return reminders;
}

bool Database::updateReminderNextDue(int medicineId, long long nextDueAtEpochSeconds) {
    const char* sql = "UPDATE medicines SET next_due_at = ?, taken = 0 WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare update next due query.\n";
        return false;
    }

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(nextDueAtEpochSeconds));
    sqlite3_bind_int(stmt, 2, medicineId);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not update reminder schedule: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    return sqlite3_changes(db) > 0;
}

bool Database::disableReminder(int medicineId) {
    const char* sql = "UPDATE medicines SET enabled = 0, enable_reminder = 0, taken = 1 WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare disable reminder query.\n";
        return false;
    }

    sqlite3_bind_int(stmt, 1, medicineId);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Could not disable reminder: " << sqlite3_errmsg(db) << "\n";
        return false;
    }
    return sqlite3_changes(db) > 0;
}

std::vector<MissedReminderInfo> Database::catchUpMissedReminders(long long nowEpochSeconds) {
    std::vector<MissedReminderInfo> missedInfos;
    std::vector<DueReminder> dueReminders = loadDueReminders(nowEpochSeconds);

    for (const DueReminder& reminder : dueReminders) {
        if (!reminder.enabled) {
            continue;
        }

        if (isDurationExpired(reminder, reminder.nextDueAt)) {
            disableReminder(reminder.medicineId);
            continue;
        }

        int missedCount = 0;
        long long nextDueAt = nextDueAfterNow(reminder, nowEpochSeconds, missedCount);

        if (nextDueAt <= reminder.nextDueAt || missedCount <= 0) {
            continue;
        }

        if (isDurationExpired(reminder, nextDueAt)) {
            disableReminder(reminder.medicineId);
            continue;
        }

        if (!updateReminderNextDue(reminder.medicineId, nextDueAt)) {
            continue;
        }

        insertMissedReminderEvent(reminder, missedCount, nowEpochSeconds, nextDueAt);

        MissedReminderInfo info;
        info.medicineId = reminder.medicineId;
        info.name = reminder.name;
        info.missedCount = missedCount;
        info.nextDueAt = nextDueAt;
        missedInfos.push_back(info);
    }

    return missedInfos;
}
