# Pre-Upload: Port mit ESP32-S3 (CoreS3) finden, wenn mehrere Geräte (z. B. Tab5=P4) am USB hängen.
# Bevorzugt den in platformio.ini gesetzten upload_port, falls dort ein ESP32-S3 hängt (damit S3 und Tab5 unterscheidbar sind).
import glob
import os
import subprocess
import sys

Import("env")

def chip_is_s3(port):
    try:
        esptool_py = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
        if not os.path.isfile(esptool_py):
            esptool_py = None
        cmd = [sys.executable, esptool_py, "--port", port, "read_mac"] if esptool_py else [sys.executable, "-m", "esptool", "--port", port, "read_mac"]
        r = subprocess.run(
            cmd,
            capture_output=True, text=True, timeout=10,
            cwd=env.get("PROJECT_DIR", "."),
        )
        out = (r.stdout or "") + (r.stderr or "")
        return "ESP32-S3" in out or "esp32s3" in out.lower()
    except Exception:
        return False

def find_cores3_port():
    candidates = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    # Wenn in platformio.ini ein Port gesetzt ist und dort ein S3 hängt → diesen nehmen (CoreS3 vs Tab5 trennbar)
    configured = env.get("UPLOAD_PORT")
    if configured and configured in candidates and chip_is_s3(configured):
        return configured
    # Sonst ersten ESP32-S3 unter allen Kandidaten
    for port in candidates:
        if chip_is_s3(port):
            return port
    return None

port = find_cores3_port()
if port:
    env.Replace(UPLOAD_PORT=port)
    print("Upload-Port (CoreS3/ESP32-S3): %s" % port)
else:
    print("Hinweis: Kein ESP32-S3 gefunden. CoreS3 per USB verbinden oder upload_port angeben.")
