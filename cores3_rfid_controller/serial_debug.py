#!/usr/bin/env python3
"""Liest Serial-Port ab Geräte-Reset und schreibt DBG-Zeilen als NDJSON in die Debug-Log-Datei."""
import serial, json, time, sys, os

BAUD    = 115200
LOGFILE = "/home/bfunk/m5stamplc_display_work/.cursor/debug-405cb5.log"
TIMEOUT = 45  # Sekunden – UART-Scanner braucht ~5s, dann 15s bis Ergebnis-Reprint

# Port auto-detect
PORT = None
for candidate in ["/dev/ttyACM0", "/dev/ttyACM1"]:
    if os.path.exists(candidate):
        PORT = candidate
        break
if not PORT:
    print("FEHLER: Kein ttyACM-Port gefunden!")
    sys.exit(1)

print(f"Verbinde {PORT} @ {BAUD} Bd  (Log → {LOGFILE})")
try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
except Exception as e:
    print(f"FEHLER: {e}")
    sys.exit(1)

# CoreS3/ESP32 per DTR resetten, damit ein frischer Boot geloggt wird
ser.dtr = False
time.sleep(0.12)
ser.dtr = True
time.sleep(0.8)
ser.reset_input_buffer()

os.makedirs(os.path.dirname(LOGFILE), exist_ok=True)
start = time.time()
count = 0

with open(LOGFILE, "w") as log:
    print(f"Lese {TIMEOUT}s ... (Ctrl+C zum Abbrechen)")
    while (time.time() - start) < TIMEOUT:
        try:
            raw = ser.readline()
        except Exception:
            break
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip()
        print(line)

        hyp = "general"
        if "H1" in line or "I2C" in line or "Wire" in line or "Scan" in line or "0x2" in line:
            hyp = "H1-H2"
        if "H2" in line or "INA226" in line or "busRaw" in line or "shuntRaw" in line:
            hyp = "H2"
        if "H3" in line or "H6" in line or "H9" in line or "RFID" in line or "UART" in line:
            hyp = "H6-H9"
        if "MATCH" in line or "passiv" in line or "cmd=" in line or "SCAN ERGEBNISSE" in line:
            hyp = "H6-H9-result"
        if "Fallback" in line or "G12" in line:
            hyp = "H5"

        entry = {
            "sessionId":    "405cb5",
            "id":           f"log_{int(time.time()*1000)}_{count}",
            "timestamp":    int(time.time() * 1000),
            "location":     "firmware:serial",
            "message":      line,
            "data":         {"raw": line},
            "hypothesisId": hyp
        }
        log.write(json.dumps(entry) + "\n")
        log.flush()
        count += 1

ser.close()
print(f"\n{count} Zeilen geloggt → {LOGFILE}")
