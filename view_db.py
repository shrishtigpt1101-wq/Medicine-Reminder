import sqlite3
import sys
from pathlib import Path


def as_text(value):
    if value is None:
        return "NULL"
    return str(value)


def print_table(name, columns, rows):
    widths = [len(col) for col in columns]
    text_rows = []
    empty_label = "(empty)"

    for row in rows:
        text_row = [as_text(cell) for cell in row]
        text_rows.append(text_row)
        for i, cell in enumerate(text_row):
            widths[i] = max(widths[i], len(cell))

    if not text_rows:
        widths[0] = max(widths[0], len(empty_label))

    sep = "+-" + "-+-".join("-" * w for w in widths) + "-+"
    header = "| " + " | ".join(columns[i].ljust(widths[i]) for i in range(len(columns))) + " |"

    print(f"\n=== {name} ===")
    print(sep)
    print(header)
    print(sep)

    if not text_rows:
        empty_row = "| " + " | ".join(empty_label.ljust(widths[i]) if i == 0 else "".ljust(widths[i]) for i in range(len(columns))) + " |"
        print(empty_row)
    else:
        for text_row in text_rows:
            line = "| " + " | ".join(text_row[i].ljust(widths[i]) for i in range(len(columns))) + " |"
            print(line)

    print(sep)
    print(f"rows: {len(rows)}")


def main():
    default_db = Path(__file__).with_name("medicine_reminder.db")
    db_path = Path(sys.argv[1]) if len(sys.argv) > 1 else default_db

    if not db_path.exists():
        print(f"Database file not found: {db_path}")
        sys.exit(1)

    conn = sqlite3.connect(str(db_path))
    cur = conn.cursor()

    tables = [
        row[0]
        for row in cur.execute(
            "SELECT name FROM sqlite_master WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%' ORDER BY name"
        )
    ]

    if not tables:
        print("No user tables found in database.")
        conn.close()
        return

    print(f"Database: {db_path}")
    for table_name in tables:
        safe_name = table_name.replace('"', '""')
        query = f'SELECT * FROM "{safe_name}"'
        table_cur = conn.execute(query)
        columns = [desc[0] for desc in table_cur.description]
        rows = table_cur.fetchall()
        print_table(table_name, columns, rows)

    conn.close()


if __name__ == "__main__":
    main()
