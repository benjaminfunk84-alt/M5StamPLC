# Pre-Upload: Port mit ESP32-S3 (CoreS3) finden, wenn mehrere Geräte (z. B. Tab5=P4) am USB hängen.
import glob
import os
import subprocess
import sys

Import("env")

def find_cores3_port():
    candidates = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    # PlatformIO's esptool (gleicher Pfad wie beim Upload)
    esptool_py = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
    if not os.path.isfile(esptool_py):
        esptool_py = None  # Fallback: -m esptool
    for port in candidates:
        try:
            cmd = [sys.executable, esptool_py, "--port", port, "read_mac"] if esptool_py else [sys.executable, "-m", "esptool", "--port", port, "read_mac"]
            r = subprocess.run(
                cmd,
                capture_output=True, text=True, timeout=10,
                cwd=env.get("PROJECT_DIR", "."),
            )
            out = (r.stdout or "") + (r.stderr or "")
            if "ESP32-S3" in out or "esp32s3" in out.lower():
                return port
        except Exception:
            continue
    return None

port = find_cores3_port()
if port:
    env.Replace(UPLOAD_PORT=port)
    print("Upload-Port (CoreS3/ESP32-S3): %s" % port)
else:
    print("Hinweis: Kein ESP32-S3 gefunden. CoreS3 per USB verbinden oder upload_port angeben.")
