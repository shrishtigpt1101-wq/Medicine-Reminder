# Medicine-Reminder

Medicine reminder system with a C++ backend, SQLite database, and web/console interfaces.

## Main Topic

The Medicine Reminder App is designed to help users manage medication schedules efficiently.  
It improves medication adherence by giving timely reminders, tracking dosage information, and supporting personalized repeat schedules.  
Users can add their medicines, configure recurring reminders, and receive dose notifications at the scheduled time.

## About This Project

This project is a desktop/web-style implementation of the Medicine Reminder idea:

- Account system with secure password hashing
- Medicine scheduling with repeat rules (daily, selected days, every X days, and interval-based repeats)
- Reminder engine that calculates `next_due_at` from system clock and stored timestamps
- Missed-reminder catch-up after server restart
- Profile management, including profile photo upload support
- Console app (`mR.cpp`) and browser UI (`web_server.cpp` + HTML/CSS)

## Features

- Signup and login system for user accounts
- Add medicines with dosage and schedule details
- Repeat reminders for daily, selected-day, and every-X-day patterns
- Missed reminder handling when server restarts after downtime
- Profile photo upload and display in user profile

## How It Works

- Each reminder stores an absolute next trigger time in `next_due_at`
- Repeat interval is stored as `repeat_minutes` (or schedule rule fields)
- Reminder loop checks due rows where `next_due_at <= now`
- After triggering, it advances to the next due time based on repeat rules
- If server is offline, catch-up logic calculates missed intervals on restart and moves to the correct next due time

## Usage Flow

1. Create account and login.
2. Add medicine with name, dosage, date, time, and repeat settings.
3. Open medicine history/reminder pages and monitor due reminders.

## Languages and Technologies Used

- C++: core backend (`web_server.cpp`), database layer (`Database.cpp`), and console app (`mR.cpp`)
- SQLite: local database storage (`medicine_reminder.db`) via `sqlite3.c` / `sqlite3.h`
- HTML: frontend pages (`login.html`, `add-medicine.html`, `user-profile.html`, etc.)
- CSS: frontend styling (`common.css`, `style.css`, and page-specific CSS files)
- Python: utility script (`view_db.py`) for viewing DB tables

## Notes

- Main reminder logic is implemented in C++ backend files (`web_server.cpp` + `Database.cpp`).
- Frontend currently runs without JavaScript-based reminder polling.
