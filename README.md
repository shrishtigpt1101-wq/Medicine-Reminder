# Medicine-Reminder

Medicine reminder system with a C++ backend, SQLite database, and web/console interfaces.

## Languages and Technologies Used

- C++: core backend (`web_server.cpp`), database layer (`Database.cpp`), and console app (`mR.cpp`)
- SQLite: local database storage (`medicine_reminder.db`) via `sqlite3.c` / `sqlite3.h`
- HTML: frontend pages (`login.html`, `add-medicine.html`, `user-profile.html`, etc.)
- CSS: frontend styling (`common.css`, `style.css`, and page-specific CSS files)
- Python: utility script (`view_db.py`) for viewing DB tables

## Notes

- Main reminder logic is implemented in C++ backend files (`web_server.cpp` + `Database.cpp`).
- Frontend currently runs without JavaScript-based reminder polling.
