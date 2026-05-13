import socket
import mysql.connector
from datetime import datetime
import sys

# ── CONFIG ─────────────────────────────────────────────────────────────────
UDP_IP   = "0.0.0.0"
UDP_PORT = 4210

DB_HOST  = "localhost"
DB_USER  = "root"
DB_PASS  = ""
DB_NAME  = "quakesense"

# ── COOLDOWN — second layer of flood protection ────────────────────────────
# Even if the ESP32 sends packets faster than expected, this ensures
# the same status is not saved to DB more than once per N seconds.
#
# Why needed: if ESP32 resets or UDP interval glitches, this catches it.
#
COOLDOWN_SECONDS = {
    "WARNING":     10,   # save P-wave event at most once per 10s
    "EARTHQUAKE!": 10,   # save earthquake event at most once per 10s
}
# ──────────────────────────────────────────────────────────────────────────


def connect_db():
    try:
        conn = mysql.connector.connect(
            host=DB_HOST, user=DB_USER, password=DB_PASS, database=DB_NAME
        )
        return conn
    except mysql.connector.Error as err:
        print(f"[DB ERROR] {err}")
        return None


def parse_packet(raw: str):
    """
    Parses: "STATUS | D:value" or "STATUS | D:value | ETA:N"
    Returns (status, diff_value, eta_seconds) or None on failure.
    """
    try:
        parts      = [p.strip() for p in raw.strip().split("|")]
        status     = parts[0].strip()
        diff_value = float(parts[1].split(":")[1])
        eta_seconds = None
        for part in parts[2:]:
            if part.strip().startswith("ETA:"):
                eta_seconds = int(part.strip().split(":")[1])
        return status, diff_value, eta_seconds
    except Exception as e:
        print(f"[PARSE ERROR] {e} | raw='{raw}'")
        return None


def save_to_db(db, cursor, status, diff_value):
    sql = """
        INSERT INTO seismic_data (status, diff_value, recorded_at)
        VALUES (%s, %s, %s)
    """
    try:
        cursor.execute(sql, (status, diff_value, datetime.now()))
        db.commit()
        return True
    except mysql.connector.Error as err:
        print(f"[DB SAVE ERROR] {err}")
        return False


def main():
    print("=" * 58)
    print("  SeismoGuard — Receiver  (flood-protected)")
    print("  WARNING    (P-Wave) → saved, max 1 per 10s")
    print("  EARTHQUAKE!(S-Wave) → saved, max 1 per 10s")
    print("  NORMAL              → logged only, never saved")
    print("=" * 58)

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
    print(f"[*] Waiting for ESP32 data...\n")
    print("-" * 58)

    # Tracks the last time each status was saved to DB
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
            wave = "P-WAVE" if status == "WARNING" else "S-WAVE" if status == "EARTHQUAKE!" else "NORMAL"
            eta_str = f" | ETA:{eta_seconds}s" if eta_seconds else ""

            print(f"[{timestamp}] {wave:<8} {status:<12} D:{diff_value:.4f}{eta_str}")

            if status in ("WARNING", "EARTHQUAKE!"):
                now_ts = datetime.now().timestamp()
                cooldown = COOLDOWN_SECONDS[status]
                elapsed  = now_ts - last_saved[status]

                if elapsed < cooldown:
                    # Too soon — skip this one
                    remaining = cooldown - elapsed
                    print(f"  ⏳ Cooldown active — skip ({remaining:.1f}s left)")
                else:
                    # Cooldown passed — save it
                    if save_to_db(db, cursor, status, diff_value):
                        last_saved[status] = now_ts
                        print(f"  ✔  Saved to DB")
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