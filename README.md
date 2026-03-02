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

## Languages and Technologies Used

- C++: core backend (`web_server.cpp`), database layer (`Database.cpp`), and console app (`mR.cpp`)
- SQLite: local database storage (`medicine_reminder.db`) via `sqlite3.c` / `sqlite3.h`
- HTML: frontend pages (`login.html`, `add-medicine.html`, `user-profile.html`, etc.)
- CSS: frontend styling (`common.css`, `style.css`, and page-specific CSS files)
- Python: utility script (`view_db.py`) for viewing DB tables

## Notes

- Main reminder logic is implemented in C++ backend files (`web_server.cpp` + `Database.cpp`).
- Frontend currently runs without JavaScript-based reminder polling.
