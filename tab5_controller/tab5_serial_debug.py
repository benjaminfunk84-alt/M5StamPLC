#!/usr/bin/env python3
"""Liest den Seriellen Port des Tab5 und schreibt Zeilen als NDJSON mit Zeitstempel in eine eigene Log-Datei."""
import serial, json, time, sys, os

BAUD    = 115200
LOGFILE = "/home/bfunk/m5stamplc_display_work/.cursor/debug-tab5-405cb5.log"
TIMEOUT = 55  # Sekunden – etwas länger als CoreS3-Log, damit Überlappung sicher ist

#!FIXED: Tab5 an /dev/ttyACM0
# Port auto-detect – Tab5 hängt aktuell in der Regel an /dev/ttyACM0
PORT = None
for candidate in ["/dev/ttyACM0", "/dev/ttyACM1"]:
    if os.path.exists(candidate):
        PORT = candidate
        break
if not PORT:
    print("FEHLER: Kein ttyACM-Port für Tab5 gefunden!")
    sys.exit(1)

print(f"Verbinde Tab5 {PORT} @ {BAUD} Bd  (Log → {LOGFILE})")
try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
except Exception as e:
    print(f"FEHLER: {e}")
    sys.exit(1)

os.makedirs(os.path.dirname(LOGFILE), exist_ok=True)
start = time.time()
count = 0

with open(LOGFILE, "w") as log:
    print(f"Lese {TIMEOUT}s vom Tab5 ... (Ctrl+C zum Abbrechen)")
    while (time.time() - start) < TIMEOUT:
        try:
            raw = ser.readline()
        except Exception:
            break
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip()
        print(line)

        entry = {
            "sessionId":    "405cb5",
            "id":           f"tab5_{int(time.time()*1000)}_{count}",
            "timestamp":    int(time.time() * 1000),
            "location":     "tab5:serial",
            "message":      line,
            "data":         {"raw": line},
            "hypothesisId": "tab5"
        }
        log.write(json.dumps(entry) + "\n")
        log.flush()
        count += 1

ser.close()
print(f"\n{count} Zeilen geloggt → {LOGFILE}")

