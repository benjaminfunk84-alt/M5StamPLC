# Pre-Upload: Port mit ESP32-P4 (Tab5) finden, wenn mehrere Geräte (z. B. CoreS3=S3) am USB hängen.
import glob
import os
import subprocess
import sys

Import("env")

def find_tab5_port():
    candidates = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    esptool_py = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
    if not os.path.isfile(esptool_py):
        esptool_py = None
    for port in candidates:
        try:
            cmd = [sys.executable, esptool_py, "--port", port, "read_mac"] if esptool_py else [sys.executable, "-m", "esptool", "--port", port, "read_mac"]
            r = subprocess.run(
                cmd,
                capture_output=True, text=True, timeout=10,
                cwd=env.get("PROJECT_DIR", "."),
            )
            out = (r.stdout or "") + (r.stderr or "")
            if "ESP32-P4" in out or "esp32-p4" in out.lower():
                return port
        except Exception:
            continue
    return None

port = find_tab5_port()
if port:
    env.Replace(UPLOAD_PORT=port)
    print("Upload-Port (Tab5/ESP32-P4): %s" % port)
else:
    print("Hinweis: Kein ESP32-P4 gefunden. Tab5 per USB verbinden oder upload_port angeben.")
