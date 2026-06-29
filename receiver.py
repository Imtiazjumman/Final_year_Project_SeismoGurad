import socket
import mysql.connector
from datetime import datetime, timedelta
import sys

# ── CONFIG ─────────────────────────────────────────────────────────────────
UDP_IP   = "0.0.0.0"
UDP_PORT = 4210

DB_HOST  = "localhost"
DB_USER  = "root"
DB_PASS  = ""
DB_NAME  = "quakesense"

# ── ALERT DISPLAY DURATION ─────────────────────────────────────────────────
# Each WARNING/EARTHQUAKE! row written to DB includes an alert_until
# timestamp = recorded_at + ALERT_DISPLAY_SECONDS.
ALERT_DISPLAY_SECONDS = {
    "WARNING":     8,    # matches new P_TO_S_SECS in firmware
    "EARTHQUAKE!": 10,   # show earthquake alert for 10 seconds
}

# ── COOLDOWN ───────────────────────────────────────────────────────────────
# Reduced from 10s to 5s — allows faster re-testing during demo
COOLDOWN_SECONDS = {
    "WARNING":     5,
    "EARTHQUAKE!": 5,
}


def connect_db():
    try:
        conn = mysql.connector.connect(
            host=DB_HOST, user=DB_USER, password=DB_PASS, database=DB_NAME
        )
        return conn
    except mysql.connector.Error as err:
        print(f"[DB ERROR] {err}")
        return None


def ensure_schema(db, cursor):
    """
    Make sure seismic_data has the alert_until column.
    Safe to run on an existing table — does nothing if column exists.
    """
    try:
        cursor.execute("""
            ALTER TABLE seismic_data
            ADD COLUMN IF NOT EXISTS alert_until DATETIME NULL
        """)
        db.commit()
        print("[*] Schema OK — alert_until column present.")
    except mysql.connector.Error as err:
        try:
            cursor.execute("""
                SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS
                WHERE TABLE_SCHEMA = %s
                  AND TABLE_NAME   = 'seismic_data'
                  AND COLUMN_NAME  = 'alert_until'
            """, (DB_NAME,))
            (count,) = cursor.fetchone()
            if count == 0:
                cursor.execute(
                    "ALTER TABLE seismic_data ADD COLUMN alert_until DATETIME NULL"
                )
                db.commit()
                print("[*] alert_until column added.")
            else:
                print("[*] Schema OK — alert_until column already exists.")
        except mysql.connector.Error as err2:
            print(f"[SCHEMA WARNING] Could not add alert_until: {err2}")
            print("    Add it manually:  ALTER TABLE seismic_data ADD COLUMN alert_until DATETIME NULL;")


def parse_packet(raw: str):
    """
    Parses: "STATUS | D:value" or "STATUS | D:value | ETA:N"
    Returns (status, diff_value, eta_seconds) or None on failure.
    """
    try:
        parts       = [p.strip() for p in raw.strip().split("|")]
        status      = parts[0].strip()
        diff_value  = float(parts[1].split(":")[1])
        eta_seconds = None
        for part in parts[2:]:
            if part.strip().startswith("ETA:"):
                eta_seconds = int(part.strip().split(":")[1])
        return status, diff_value, eta_seconds
    except Exception as e:
        print(f"[PARSE ERROR] {e} | raw='{raw}'")
        return None


def save_to_db(db, cursor, status, diff_value):
    """
    Inserts a row with recorded_at = now and alert_until = now + display window.
    """
    now         = datetime.now()
    display_s   = ALERT_DISPLAY_SECONDS.get(status, 10)
    alert_until = now + timedelta(seconds=display_s)

    sql = """
        INSERT INTO seismic_data (status, diff_value, recorded_at, alert_until)
        VALUES (%s, %s, %s, %s)
    """
    try:
        cursor.execute(sql, (status, diff_value, now, alert_until))
        db.commit()
        return True
    except mysql.connector.Error as err:
        print(f"[DB SAVE ERROR] {err}")
        return False


def main():
    print("=" * 62)
    print("  SeismoGuard — Receiver  (flood-protected + timed alerts)")
    print()
    print("  P_TO_S_SECS   : 8s  (firmware window)")
    print("  WARNING cooldown  : 5s")
    print("  EARTHQUAKE cooldown: 5s")
    print()
    print("  WARNING    (P-Wave) → saved, alert shown for 8s")
    print("  EARTHQUAKE!(S-Wave) → saved, alert shown for 10s")
    print("  NORMAL              → logged only, never saved")
    print("=" * 62)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        sock.bind((UDP_IP, UDP_PORT))
    except OSError as e:
        print(f"[FATAL] Cannot bind port {UDP_PORT}: {e}")
        print("  Tip: taskkill /F /IM python.exe  to clear old processes.")
        return

    print(f"[*] Listening on UDP port {UDP_PORT}...")

    db = connect_db()
    if not db:
        print("[FATAL] Database connection failed.")
        return

    cursor = db.cursor()
    print(f"[*] Connected to MySQL '{DB_NAME}'")

    ensure_schema(db, cursor)

    print(f"[*] Waiting for ESP32 data...\n")
    print("-" * 62)

    last_saved = {
        "WARNING":     0.0,
        "EARTHQUAKE!": 0.0,
    }

    try:
        while True:
            data, addr = sock.recvfrom(1024)
            raw = data.decode("utf-8").strip()
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

            result = parse_packet(raw)
            if not result:
                print(f"[{timestamp}] ✘ Bad packet: '{raw}'")
                continue

            status, diff_value, eta_seconds = result
            wave    = "P-WAVE" if status == "WARNING" else "S-WAVE" if status == "EARTHQUAKE!" else "NORMAL"
            eta_str = f" | ETA:{eta_seconds}s" if eta_seconds else ""

            print(f"[{timestamp}] {wave:<8} {status:<12} D:{diff_value:.4f}{eta_str}")

            if status in ("WARNING", "EARTHQUAKE!"):
                now_ts   = datetime.now().timestamp()
                cooldown = COOLDOWN_SECONDS[status]
                elapsed  = now_ts - last_saved[status]

                if elapsed < cooldown:
                    remaining = cooldown - elapsed
                    print(f"  ⏳ Cooldown active — skip ({remaining:.1f}s left)")
                else:
                    if save_to_db(db, cursor, status, diff_value):
                        last_saved[status] = now_ts
                        display_s = ALERT_DISPLAY_SECONDS[status]
                        print(f"  ✔  Saved to DB  (alert_until = now + {display_s}s)")
                    else:
                        print(f"  ✘  DB save failed")
            else:
                print(f"  ⏭  NORMAL — not saved")

    except KeyboardInterrupt:
        print("\n[!] Stopped by user (Ctrl+C)")
    finally:
        if 'cursor' in locals(): cursor.close()
        if db and db.is_connected(): db.close()
        sock.close()
        print("[*] All connections closed.")


if __name__ == "__main__":
    main()