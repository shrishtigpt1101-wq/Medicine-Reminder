// Models.h
#pragma once

#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

class Medicine {
private:
    int id;
    std::string name;
    std::string dosage;
    std::string time;
    bool taken;
    long long nextDueAt;
    int repeatMinutes;
    int repeatValue;
    std::string repeatUnit;
    bool enabled;
    std::string frequency;
    std::string repeatMode;
    int repeatEveryXDays;
    std::string selectedDays;
    std::string startDate;
    std::string durationType;
    int durationDays;
    long long endAt;

public:
    Medicine(std::string n,
             std::string d,
             std::string t,
             bool isTaken = false,
             int medicineId = -1,
             long long nextDueAtEpoch = 0,
             int repeatIntervalMinutes = 0,
             int repeatIntervalValue = 0,
             std::string repeatIntervalUnit = "",
             bool isEnabled = true,
             std::string reminderFrequency = "specific_times",
             std::string reminderRepeatMode = "every_day",
             int reminderRepeatEveryXDays = 1,
             std::string reminderSelectedDays = "",
             std::string reminderStartDate = "",
             std::string reminderDurationType = "continuous",
             int reminderDurationDays = 0,
             long long reminderEndAt = 0)
        : id(medicineId),
          name(n),
          dosage(d),
          time(t),
          taken(isTaken),
          nextDueAt(nextDueAtEpoch),
          repeatMinutes(repeatIntervalMinutes),
          repeatValue(repeatIntervalValue),
          repeatUnit(repeatIntervalUnit),
          enabled(isEnabled),
          frequency(reminderFrequency),
          repeatMode(reminderRepeatMode),
          repeatEveryXDays(reminderRepeatEveryXDays),
          selectedDays(reminderSelectedDays),
          startDate(reminderStartDate),
          durationType(reminderDurationType),
          durationDays(reminderDurationDays),
          endAt(reminderEndAt) {}

    void markTaken() { taken = true; }

    void display(int index) const {
        std::cout << index + 1 << ". " << std::left << std::setw(15) << name
                  << " | Dosage: " << std::setw(12) << dosage
                  << " | Time: " << std::setw(8) << time
                  << " | Status: " << (taken ? "Taken" : "Pending") << "\n";
    }

    int getId() const { return id; }
    void setId(int medicineId) { id = medicineId; }
    std::string getName() const { return name; }
    std::string getDosage() const { return dosage; }
    std::string getTime() const { return time; }
    bool isTaken() const { return taken; }
    long long getNextDueAt() const { return nextDueAt; }
    int getRepeatMinutes() const { return repeatMinutes; }
    int getRepeatValue() const { return repeatValue; }
    std::string getRepeatUnit() const { return repeatUnit; }
    bool isEnabled() const { return enabled; }
    std::string getFrequency() const { return frequency; }
    std::string getRepeatMode() const { return repeatMode; }
    int getRepeatEveryXDays() const { return repeatEveryXDays; }
    std::string getSelectedDays() const { return selectedDays; }
    std::string getStartDate() const { return startDate; }
    std::string getDurationType() const { return durationType; }
    int getDurationDays() const { return durationDays; }
    long long getEndAt() const { return endAt; }
};

class User {
public:
    std::string name;
    int age = 0;
    std::string gender;
    std::string medicalIssue;
    std::string contactNumber;
    std::string emergencyContact;
    std::string profilePhotoPath;

    void inputDetails() {
        std::cout << "\n--- Enter User Details ---\n";
        std::cout << "Enter Full Name: ";
        std::getline(std::cin, name);

        std::cout << "Enter Age: ";
        while (!(std::cin >> age)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "Invalid age. Enter Age: ";
        }
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        std::cout << "Enter Gender: ";
        std::getline(std::cin, gender);

        std::cout << "Enter Medical Issue (press enter to skip): ";
        std::getline(std::cin, medicalIssue);

        std::cout << "Enter Contact Number: ";
        std::getline(std::cin, contactNumber);

        std::cout << "Enter Emergency Contact Number (press enter to skip): ";
        std::getline(std::cin, emergencyContact);
    }
};
