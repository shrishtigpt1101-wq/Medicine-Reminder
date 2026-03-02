# Medicine Reminder

A local medicine reminder system built with C++, SQLite, and a lightweight web interface.

## Package and App Info

- App name: `Medicine Reminder`
- Repository: `Medicine-Reminder`
- App type: local desktop/web project
- Build output: native executables (`mR.exe`, `web_server.exe`)
- Package manager: not used in this project

## Requirements (What You Need)

- Windows 10/11 (web server uses WinSock)
- MinGW-w64 tools (`gcc`, `g++`) with C++17 support
- Python 3 (optional, only for `view_db.py`)
- Any modern browser

## Tech Stack

- C++17 (`mR.cpp`, `web_server.cpp`, `Database.cpp`)
- SQLite (`sqlite3.c`, `sqlite3.h`, `medicine_reminder.db`)
- HTML/CSS frontend pages (`*.html`, `*.css`)
- Python utility script (`view_db.py`)

## What Is Implemented

- Account signup and login
- Password hashing in backend account flow
- Add medicine with dosage, date, time, repeat, and duration options
- Reminder scheduling using `next_due_at` with repeat rules
- Missed-reminder catch-up after server restart
- Medicine history actions (mark taken, delete)
- User profile update
- Profile photo upload and display (`jpg`, `jpeg`, `png`, `webp`, max 2 MB)
- Account-specific reminder filtering in server runtime

## Build and Test (CLI)

Build commands from project root:

```powershell
gcc -c sqlite3.c -o sqlite3.o
g++ -std=c++17 mR.cpp Database.cpp sqlite3.o -o mR.exe
g++ -std=c++17 web_server.cpp Database.cpp sqlite3.o -lws2_32 -o web_server.exe
```

Test status:

- No automated test suite is present in this repository.
- Current testing is manual (run app flows and validate behavior).

## How To Run

Web app:

```powershell
.\web_server.exe
```

Open:

- `http://localhost:8080/login.html`

Console app:

```powershell
.\mR.exe
```

Optional DB view:

```powershell
python view_db.py
```

## Project Structure

- `web_server.cpp`: HTTP server, routes, session handling, reminder loop
- `Database.cpp` / `Database.h`: SQLite schema, auth, user, medicine, reminder DB logic
- `mR.cpp`: console-based application workflow
- `Models.h`: `Medicine` and `User` model definitions
- `sqlite3.c` / `sqlite3.h`: embedded SQLite source
- `uploads/profile/`: uploaded profile photos
- `medicine_reminder.db`: local SQLite database

## Project Architecture

- Presentation layer: server-rendered HTML/CSS pages and console UI
- Application layer: C++ logic in `web_server.cpp` and `mR.cpp`
- Data access layer: `Database.cpp` / `Database.h`
- Storage layer: SQLite DB file + local uploads folder
