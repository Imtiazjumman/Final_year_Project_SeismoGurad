import socket
import mysql.connector
from datetime import datetime
import sys
import io

# Fix for potential encoding issues in Windows terminal
#sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

# ── CONFIG ────────────────────────────────────────────────
UDP_IP   = "0.0.0.0"  # Listen on all available network interfaces
UDP_PORT = 4210

DB_HOST  = "localhost"
DB_USER  = "root"
DB_PASS  = ""
DB_NAME  = "quakesense"
# ─────────────────────────────────────────────────────────

def connect_db():
    """Establishes connection to the MySQL database."""
    try:
        conn = mysql.connector.connect(
            host     = DB_HOST,
            user     = DB_USER,
            password = DB_PASS,
            database = DB_NAME
        )
        return conn
    except mysql.connector.Error as err:
        print(f"[DB ERROR] Could not connect: {err}")
        return None

def parse_packet(raw: str):
    """Parses incoming string: 'STATUS | D:value'"""
    try:
        parts      = raw.strip().split("|")
        status     = parts[0].strip()
        diff_str   = parts[1].strip()
        # Extract numeric value after 'D:'
        diff_value = float(diff_str.split(":")[1])
        return status, diff_value
    except Exception as e:
        print(f"[PARSE ERROR] {e} | raw='{raw}'")
        return None

def save_to_db(db, cursor, status, diff_value):
    """Inserts record into database and handles reconnection if needed."""
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
    # 1. Setup UDP Socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    # FIX: Allow immediate reuse of the port to solve WinError 10048
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        sock.bind((UDP_IP, UDP_PORT))
    except OSError as e:
        print(f"[FATAL] Could not bind to port {UDP_PORT}: {e}")
        print("Tip: Run 'taskkill /F /IM python.exe' in terminal to clear old processes.")
        return

    print(f"[*] Listening for UDP packets on port {UDP_PORT}...")

    # 2. Setup Database
    db = connect_db()
    if not db:
        return
    
    cursor = db.cursor()
    print(f"[*] Connected to MySQL database '{DB_NAME}'")
    print(f"[*] Monitoring for WARNING and EARTHQUAKE events...")
    print("-" * 50)

    # 3. Main Loop
    try:
        while True:
            # Wait for data
            data, addr = sock.recvfrom(1024)
            raw = data.decode("utf-8")
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

            print(f"[{timestamp}] From {addr[0]} → {raw}")

            result = parse_packet(raw)
            if result:
                status, diff_value = result

                # Only save WARNING and EARTHQUAKE!
                if status in ("WARNING", "EARTHQUAKE!"):
                    if save_to_db(db, cursor, status, diff_value):
                        print(f"  ✔ Saved → {status} | Diff: {diff_value:.4f}")
                else:
                    print(f"  ⏭ Skipped (NORMAL)")
            else:
                print("  ✘ Skipped (Parse Failed)")

    except KeyboardInterrupt:
        print("\n[!] Stopped by user.")
    finally:
        if 'cursor' in locals(): cursor.close()
        if 'db' in locals(): db.close()
        sock.close()
        print("[*] Connections closed safely.")

if __name__ == "__main__":
    main()