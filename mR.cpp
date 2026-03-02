// mR.cpp
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <cctype>

#include "Database.h"
#include "Models.h"

using namespace std;

static string toLowerCopyLocal(string value) {
    transform(value.begin(), value.end(), value.begin(),
              [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return value;
}

static bool isSupportedRepeatUnitLocal(const string& unit) {
    return unit == "minute" ||
           unit == "hour" ||
           unit == "day" ||
           unit == "week" ||
           unit == "month" ||
           unit == "year";
}

static bool isSupportedRepeatModeLocal(const string& repeatMode) {
    return repeatMode == "every_day" ||
           repeatMode == "selected_days" ||
           repeatMode == "every_x_days";
}

static bool isSupportedDurationTypeLocal(const string& durationType) {
    return durationType == "continuous" || durationType == "number_of_days";
}

static bool parseTime24Local(const string& value, int& hour, int& minute) {
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

static bool parseDateYmdLocal(const string& value, int& year, int& month, int& day) {
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

static long long buildEpochFromLocalDateTimeLocal(int year, int month, int day, int hour, int minute) {
    tm due{};
    due.tm_year = year - 1900;
    due.tm_mon = month - 1;
    due.tm_mday = day;
    due.tm_hour = hour;
    due.tm_min = minute;
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

static long long addIntervalToEpochLocal(long long epochSeconds, int value, const string& unit) {
    if (value < 0) {
        return epochSeconds;
    }

    if (unit == "minute") return epochSeconds + (static_cast<long long>(value) * 60LL);
    if (unit == "hour") return epochSeconds + (static_cast<long long>(value) * 3600LL);
    if (unit == "day") return epochSeconds + (static_cast<long long>(value) * 86400LL);
    if (unit == "week") return epochSeconds + (static_cast<long long>(value) * 604800LL);

    time_t raw = static_cast<time_t>(epochSeconds);
    tm* local = localtime(&raw);
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

static long long nextDueAfterNowLocal(long long dueEpochSeconds,
                                      int repeatValue,
                                      const string& repeatUnit,
                                      long long nowEpochSeconds) {
    if (repeatValue <= 0 || !isSupportedRepeatUnitLocal(repeatUnit)) {
        return dueEpochSeconds;
    }

    if (repeatUnit == "minute" || repeatUnit == "hour" || repeatUnit == "day" || repeatUnit == "week") {
        long long intervalSeconds = addIntervalToEpochLocal(0, repeatValue, repeatUnit);
        if (intervalSeconds <= 0) {
            return dueEpochSeconds;
        }
        if (dueEpochSeconds > nowEpochSeconds) {
            return dueEpochSeconds;
        }
        long long missed = ((nowEpochSeconds - dueEpochSeconds) / intervalSeconds) + 1LL;
        return dueEpochSeconds + (missed * intervalSeconds);
    }

    long long next = dueEpochSeconds;
    int guard = 0;
    while (next <= nowEpochSeconds && guard < 500000) {
        long long advanced = addIntervalToEpochLocal(next, repeatValue, repeatUnit);
        if (advanced <= next) {
            break;
        }
        next = advanced;
        ++guard;
    }
    return next;
}

static int weekdayToIndexLocal(const string& tokenRaw) {
    const string token = toLowerCopyLocal(tokenRaw);
    if (token == "sun" || token == "sunday") return 0;
    if (token == "mon" || token == "monday") return 1;
    if (token == "tue" || token == "tues" || token == "tuesday") return 2;
    if (token == "wed" || token == "wednesday") return 3;
    if (token == "thu" || token == "thur" || token == "thurs" || token == "thursday") return 4;
    if (token == "fri" || token == "friday") return 5;
    if (token == "sat" || token == "saturday") return 6;
    return -1;
}

static vector<int> parseSelectedDaysCsvLocal(const string& csv) {
    vector<int> result;
    bool used[7] = {false, false, false, false, false, false, false};
    size_t start = 0;
    while (start <= csv.size()) {
        size_t comma = csv.find(',', start);
        if (comma == string::npos) {
            comma = csv.size();
        }

        string token = csv.substr(start, comma - start);
        token.erase(remove_if(token.begin(), token.end(),
                              [](unsigned char c) { return std::isspace(c) != 0; }),
                    token.end());
        int day = weekdayToIndexLocal(token);
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

static long long nextSelectedDayOccurrenceLocal(long long dueEpochSeconds, const vector<int>& selectedDays) {
    if (selectedDays.empty()) {
        return dueEpochSeconds;
    }

    time_t raw = static_cast<time_t>(dueEpochSeconds);
    tm* local = localtime(&raw);
    if (!local) {
        return dueEpochSeconds;
    }
    tm base = *local;

    for (int offset = 1; offset <= 14; ++offset) {
        tm candidate = base;
        candidate.tm_mday += offset;
        candidate.tm_isdst = -1;
        time_t converted = mktime(&candidate);
        if (converted == static_cast<time_t>(-1)) {
            continue;
        }

        tm* check = localtime(&converted);
        if (!check) {
            continue;
        }

        if (find(selectedDays.begin(), selectedDays.end(), check->tm_wday) != selectedDays.end()) {
            return static_cast<long long>(converted);
        }
    }

    return dueEpochSeconds;
}

static long long nextDueAfterNowByRuleLocal(long long dueEpochSeconds,
                                            const string& repeatModeRaw,
                                            int repeatEveryXDays,
                                            const string& selectedDaysCsv,
                                            int legacyRepeatValue,
                                            const string& legacyRepeatUnit,
                                            long long nowEpochSeconds) {
    string repeatMode = toLowerCopyLocal(repeatModeRaw);

    long long next = dueEpochSeconds;
    int guard = 0;
    while (next <= nowEpochSeconds && guard < 500000) {
        long long advanced = next;
        if (repeatMode == "every_day") {
            advanced = addIntervalToEpochLocal(next, 1, "day");
        } else if (repeatMode == "every_x_days") {
            advanced = addIntervalToEpochLocal(next, repeatEveryXDays > 0 ? repeatEveryXDays : 1, "day");
        } else if (repeatMode == "selected_days") {
            advanced = nextSelectedDayOccurrenceLocal(next, parseSelectedDaysCsvLocal(selectedDaysCsv));
        } else if (legacyRepeatValue > 0 && isSupportedRepeatUnitLocal(legacyRepeatUnit)) {
            advanced = addIntervalToEpochLocal(next, legacyRepeatValue, legacyRepeatUnit);
        }

        if (advanced <= next) {
            break;
        }
        next = advanced;
        ++guard;
    }
    return next;
}

class Account {
private:
    int id = -1;
    string username;
    string password;

public:
    int getId() const { return id; }
    string getUsername() const { return username; }
    string getMaskedPassword() const { return string(password.length(), '*'); }

    bool createAccount(Database& database) {
        cout << "\n--- Create Account ---\n";
        cout << "Set Username: ";
        getline(cin, username);

        cout << "Set Password: ";
        getline(cin, password);

        if (database.createAccount(username, password, id)) {
            cout << "\nAccount created successfully.\n";
            return true;
        }

        cout << "\nFailed to create account. Username may already exist.\n";
        return false;
    }

    bool login(Database& database) {
        cout << "\n--- Login ---\n";
        cout << "Username: ";
        getline(cin, username);

        cout << "Password: ";
        getline(cin, password);

        if (database.login(username, password, id)) {
            cout << "\nLogin successful.\n";
            return true;
        }

        cout << "\nInvalid username or password.\n";
        return false;
    }

    void changeUsername(Database& database) {
        string newUsername;
        cout << "\nEnter new username: ";
        getline(cin, newUsername);

        if (newUsername.empty()) {
            cout << "Username cannot be empty.\n";
            return;
        }

        if (database.updateUsername(id, newUsername)) {
            username = newUsername;
            cout << "Username updated.\n";
        } else {
            cout << "Failed to update username.\n";
        }
    }

    void changePassword(Database& database) {
        string oldPass;
        string newPass;

        cout << "\nEnter old password: ";
        getline(cin, oldPass);

        if (!database.verifyAccountPassword(id, oldPass)) {
            cout << "Wrong old password.\n";
            return;
        }

        cout << "Enter new password: ";
        getline(cin, newPass);

        if (newPass.empty()) {
            cout << "New password cannot be empty.\n";
            return;
        }

        if (database.updatePassword(id, newPass)) {
            password = newPass;
            cout << "Password changed successfully.\n";
        } else {
            cout << "Failed to change password.\n";
        }
    }
};

class MedicineReminderApp {
private:
    User user;
    Account account;
    vector<Medicine> medicines;
    Database database{"medicine_reminder.db"};

    int readChoice(const string& prompt, int minValue, int maxValue) {
        int choice;
        while (true) {
            cout << prompt;
            if (cin >> choice && choice >= minValue && choice <= maxValue) {
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
                return choice;
            }
            cin.clear();
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            cout << "Invalid choice. Try again.\n";
        }
    }

    bool authenticate() {
        while (true) {
            cout << "\n===== ACCOUNT =====\n";
            cout << "1. Create Account\n";
            cout << "2. Login\n";
            cout << "3. Exit\n";
            int choice = readChoice("Enter choice: ", 1, 3);

            if (choice == 1) {
                if (account.createAccount(database)) {
                    return true;
                }
            } else if (choice == 2) {
                if (account.login(database)) {
                    return true;
                }
            } else {
                return false;
            }
        }
    }

    void loadOrCreateProfile() {
        if (database.loadUser(account.getId(), user)) {
            return;
        }

        cout << "\nNo profile found for this account. Please enter details.\n";
        user.inputDetails();
        if (database.saveUser(account.getId(), user)) {
            cout << "Profile saved.\n";
        } else {
            cout << "Failed to save profile.\n";
        }
    }

public:
    void start() {
        if (!database.isOpen()) {
            cout << "Database connection failed. Exiting.\n";
            return;
        }

        if (!authenticate()) {
            cout << "\nGoodbye.\n";
            return;
        }

        loadOrCreateProfile();
        medicines = database.loadMedicines(account.getId());

        int choice;
        do {
            cout << "\n===== MENU =====\n";
            cout << "1. View Profile\n";
            cout << "2. Update Profile\n";
            cout << "3. Change Username\n";
            cout << "4. Change Password\n";
            cout << "5. Add Medicine\n";
            cout << "6. Show Medicines\n";
            cout << "7. Take Medicine\n";
            cout << "8. Delete Medicine from History\n";
            cout << "9. Exit\n";

            choice = readChoice("Enter choice: ", 1, 9);

            switch (choice) {
                case 1:
                    displayProfile();
                    break;
                case 2:
                    updateProfile();
                    break;
                case 3:
                    account.changeUsername(database);
                    break;
                case 4:
                    account.changePassword(database);
                    break;
                case 5:
                    addMedicine();
                    break;
                case 6:
                    showMedicines();
                    break;
                case 7:
                    takeMedicine();
                    break;
                case 8:
                    deleteMedicine();
                    break;
                case 9:
                    cout << "\nGoodbye.\n";
                    break;
                default:
                    cout << "Invalid choice.\n";
            }
        } while (choice != 9);
    }

    void displayProfile() const {
        cout << "\n========================================\n";
        cout << setw(25) << "USER PROFILE" << "\n";
        cout << "========================================\n";
        cout << left;
        cout << setw(20) << "Username" << ": " << account.getUsername() << "\n";
        cout << setw(20) << "Password" << ": " << account.getMaskedPassword() << "\n";
        cout << setw(20) << "Name" << ": " << user.name << "\n";
        cout << setw(20) << "Age" << ": " << user.age << "\n";
        cout << setw(20) << "Gender" << ": " << user.gender << "\n";
        cout << setw(20) << "Medical Issue" << ": "
             << (user.medicalIssue.empty() ? "None" : user.medicalIssue) << "\n";
        cout << setw(20) << "Contact Number" << ": " << user.contactNumber << "\n";
        cout << setw(20) << "Emergency Contact" << ": "
             << (user.emergencyContact.empty() ? "None" : user.emergencyContact) << "\n";
        cout << "========================================\n";
    }

    void updateProfile() {
        user.inputDetails();
        if (database.saveUser(account.getId(), user)) {
            cout << "\nProfile updated successfully.\n";
        } else {
            cout << "\nFailed to update profile.\n";
        }
    }

    void addMedicine() {
        string name;
        string dosage;
        string startDate;
        string time24h;
        int timeHour = -1;
        int timeMinute = -1;
        int dateYear = 0;
        int dateMonth = 0;
        int dateDay = 0;
        string enableReminderRaw;
        bool enableReminder = true;
        string frequency = "specific_times";
        string repeatMode = "every_day";
        int repeatEveryXDays = 1;
        string selectedDays;
        string durationType = "continuous";
        int durationDays = 0;

        cout << "\nEnter Medicine Name: ";
        getline(cin, name);

        cout << "Enter Dosage (e.g. 1 tablet): ";
        getline(cin, dosage);

        cout << "Enter time (24-hour, HH:MM): ";
        while (true) {
            getline(cin, time24h);
            if (parseTime24Local(time24h, timeHour, timeMinute)) {
                break;
            }
            cout << "Invalid time. Enter HH:MM (24-hour): ";
        }

        cout << "Enter start date (YYYY-MM-DD): ";
        while (true) {
            getline(cin, startDate);
            if (parseDateYmdLocal(startDate, dateYear, dateMonth, dateDay)) {
                break;
            }
            cout << "Invalid date. Enter YYYY-MM-DD: ";
        }

        cout << "Enable reminder? (on/off): ";
        while (true) {
            getline(cin, enableReminderRaw);
            enableReminderRaw = toLowerCopyLocal(enableReminderRaw);
            if (enableReminderRaw == "on" || enableReminderRaw == "yes" || enableReminderRaw == "true") {
                enableReminder = true;
                break;
            }
            if (enableReminderRaw == "off" || enableReminderRaw == "no" || enableReminderRaw == "false") {
                enableReminder = false;
                break;
            }
            cout << "Invalid option. Enter on or off: ";
        }

        cout << "Repeat mode (every_day / selected_days / every_x_days): ";
        while (true) {
            getline(cin, repeatMode);
            repeatMode = toLowerCopyLocal(repeatMode);
            if (isSupportedRepeatModeLocal(repeatMode)) {
                break;
            }
            cout << "Invalid mode. Enter every_day, selected_days, or every_x_days: ";
        }

        if (repeatMode == "selected_days") {
            cout << "Enter selected days as CSV (e.g. mon,wed,fri): ";
            while (true) {
                getline(cin, selectedDays);
                selectedDays = toLowerCopyLocal(selectedDays);
                if (!parseSelectedDaysCsvLocal(selectedDays).empty()) {
                    break;
                }
                cout << "Invalid days. Enter CSV like mon,wed,fri: ";
            }
        } else if (repeatMode == "every_x_days") {
            while (true) {
                cout << "Enter X (repeat every X days): ";
                if (cin >> repeatEveryXDays && repeatEveryXDays > 0) {
                    cin.ignore(numeric_limits<streamsize>::max(), '\n');
                    break;
                }
                cin.clear();
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
                cout << "Invalid number.\n";
            }
        }

        cout << "Duration type (continuous / number_of_days): ";
        while (true) {
            getline(cin, durationType);
            durationType = toLowerCopyLocal(durationType);
            if (isSupportedDurationTypeLocal(durationType)) {
                break;
            }
            cout << "Invalid option. Enter continuous or number_of_days: ";
        }

        if (durationType == "number_of_days") {
            while (true) {
                cout << "Enter duration days: ";
                if (cin >> durationDays && durationDays > 0) {
                    cin.ignore(numeric_limits<streamsize>::max(), '\n');
                    break;
                }
                cin.clear();
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
                cout << "Invalid number.\n";
            }
        }

        long long nowEpochSeconds = static_cast<long long>(
            chrono::duration_cast<chrono::seconds>(
                chrono::system_clock::now().time_since_epoch()).count());

        long long nextDueAt = buildEpochFromLocalDateTimeLocal(dateYear, dateMonth, dateDay, timeHour, timeMinute);
        if (nextDueAt <= 0) {
            cout << "\nInvalid start date/time.\n";
            return;
        }

        long long endAt = 0;
        if (durationType == "number_of_days") {
            long long endExclusive = addIntervalToEpochLocal(nextDueAt, durationDays, "day");
            if (endExclusive <= nextDueAt) {
                cout << "\nInvalid duration.\n";
                return;
            }
            endAt = endExclusive - 1;
        }

        int repeatValue = 1;
        string repeatUnit = "day";
        int repeatMinutes = 1440;
        if (repeatMode == "every_x_days") {
            repeatValue = repeatEveryXDays;
            repeatUnit = "day";
            long long repeatMinutes64 = static_cast<long long>(repeatEveryXDays) * 1440LL;
            if (repeatMinutes64 > static_cast<long long>(numeric_limits<int>::max())) {
                cout << "\nRepeat interval is too large.\n";
                return;
            }
            repeatMinutes = static_cast<int>(repeatMinutes64);
        } else if (repeatMode == "selected_days") {
            repeatValue = 1;
            repeatUnit = "week";
            repeatMinutes = 10080;
        }

        if (enableReminder && nextDueAt <= nowEpochSeconds) {
            nextDueAt = nextDueAfterNowByRuleLocal(
                nextDueAt,
                repeatMode,
                repeatEveryXDays,
                selectedDays,
                repeatValue,
                repeatUnit,
                nowEpochSeconds);
        }

        if (enableReminder && nextDueAt <= nowEpochSeconds) {
            cout << "\nStart date/time must produce a future reminder.\n";
            return;
        }

        if (enableReminder && endAt > 0 && nextDueAt > endAt) {
            cout << "\nReminder duration has already ended.\n";
            return;
        }

        Medicine medicine(name,
                          dosage,
                          time24h,
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
        int medicineId = database.addMedicine(account.getId(), medicine);
        if (medicineId == -1) {
            cout << "\nFailed to save medicine.\n";
            return;
        }

        medicine.setId(medicineId);
        medicines.push_back(medicine);
        cout << "\nMedicine added successfully.\n";
    }

    void showMedicines() const {
        if (medicines.empty()) {
            cout << "\nNo medicines added yet.\n";
            return;
        }

        cout << "\nMedicine List:\n";
        for (size_t i = 0; i < medicines.size(); ++i) {
            medicines[i].display(static_cast<int>(i));
        }
    }

    void takeMedicine() {
        if (medicines.empty()) {
            cout << "\nNo medicines available.\n";
            return;
        }

        showMedicines();
        int choice = readChoice("\nEnter medicine number to mark as taken: ", 1,
                                static_cast<int>(medicines.size()));

        Medicine& selected = medicines[static_cast<size_t>(choice - 1)];
        if (selected.isTaken()) {
            cout << "\nThis medicine is already marked as taken.\n";
            return;
        }

        if (database.markMedicineTaken(selected.getId())) {
            selected.markTaken();
            cout << "\nMedicine marked as taken.\n";
        } else {
            cout << "\nFailed to update medicine status.\n";
        }
    }

    void deleteMedicine() {
        if (medicines.empty()) {
            cout << "\nNo medicines to delete.\n";
            return;
        }

        showMedicines();
        int choice = readChoice("\nEnter medicine number to delete from history: ", 1,
                                static_cast<int>(medicines.size()));

        size_t index = static_cast<size_t>(choice - 1);
        if (database.deleteMedicine(medicines[index].getId())) {
            cout << "\nMedicine '" << medicines[index].getName() << "' deleted.\n";
            medicines.erase(medicines.begin() + static_cast<vector<Medicine>::difference_type>(index));
        } else {
            cout << "\nFailed to delete medicine.\n";
        }
    }
};

int main() {
    MedicineReminderApp app;
    app.start();
    return 0;
}
