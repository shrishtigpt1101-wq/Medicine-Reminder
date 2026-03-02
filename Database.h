// Database.h
#pragma once

#include <string>
#include <vector>

#include "Models.h"
#include "sqlite3.h"

struct DueReminder {
    int medicineId = -1;
    int accountId = -1;
    std::string name;
    std::string dosage;
    std::string timeLabel;
    bool enabled = true;
    long long nextDueAt = 0;
    int repeatMinutes = 0;
    int repeatValue = 0;
    std::string repeatUnit;
    std::string frequency;
    std::string repeatMode;
    int repeatEveryXDays = 1;
    std::string selectedDays;
    std::string startDate;
    std::string durationType;
    int durationDays = 0;
    long long endAt = 0;
};

struct MissedReminderInfo {
    int medicineId = -1;
    std::string name;
    int missedCount = 0;
    long long nextDueAt = 0;
};

class Database {
private:
    sqlite3* db = nullptr;

    static std::string columnText(sqlite3_stmt* stmt, int columnIndex);
    bool execute(const std::string& sql);
    bool hasColumn(const std::string& tableName, const std::string& columnName);
    bool addColumnIfMissing(const std::string& tableName,
                            const std::string& columnName,
                            const std::string& columnDefinition);
    bool hashPassword(const std::string& plainPassword, std::string& outHash);
    bool verifyPasswordValue(const std::string& plainPassword,
                             const std::string& storedValue,
                             bool& isValid,
                             bool& needsUpgrade);
    bool updateStoredPasswordHash(int accountId, const std::string& passwordHash);
    void insertAuditLog(int accountId, const std::string& eventType, const std::string& details);
    void insertMissedReminderEvent(const DueReminder& reminder,
                                   int missedCount,
                                   long long recordedAtEpochSeconds,
                                   long long nextDueAtEpochSeconds);

public:
    explicit Database(const std::string& fileName);
    ~Database();

    bool isOpen() const;
    bool initialize();

    bool createAccount(const std::string& username, const std::string& password, int& accountId);
    bool login(const std::string& username, const std::string& password, int& accountId);
    bool getAccountById(int accountId, std::string& username, std::string& password);
    bool verifyAccountPassword(int accountId, const std::string& password);
    bool updateUsername(int accountId, const std::string& newUsername);
    bool updatePassword(int accountId, const std::string& newPassword);

    bool saveUser(int accountId, const User& user);
    bool loadUser(int accountId, User& user);
    bool updateUserProfilePhotoPath(int accountId, const std::string& photoPath);
    bool getUserProfilePhotoPath(int accountId, std::string& photoPath);

    int addMedicine(int accountId, const Medicine& medicine);
    std::vector<Medicine> loadMedicines(int accountId);
    bool markMedicineTaken(int medicineId);
    bool deleteMedicine(int medicineId);

    std::vector<DueReminder> loadDueReminders(long long nowEpochSeconds);
    bool updateReminderNextDue(int medicineId, long long nextDueAtEpochSeconds);
    bool disableReminder(int medicineId);
    std::vector<MissedReminderInfo> catchUpMissedReminders(long long nowEpochSeconds);
};
