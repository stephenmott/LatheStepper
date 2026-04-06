#!/bin/bash
# LatheStepper upload script — always uses the correct flash partition for OTA.
# Usage:
#   ./upload.sh          — compile only
#   ./upload.sh usb      — compile + upload via USB
#   ./upload.sh wifi     — compile + upload via WiFi OTA (Pico must be running)
#
# The flash=2097152_1048576 flag is REQUIRED — it reserves 1MB for LittleFS
# so the OTA updater has somewhere to stage incoming firmware.
# Uploading without this flag will wipe the filesystem and break OTA.

FQBN="rp2040:rp2040:rpipicow:flash=2097152_1048576"
SKETCH="LatheStepper"
ARDUINO_CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"

# Read Pico IP from secrets.h if present (looks for OTA_IP, PICO_IP or OTA_HOST)
PICO_IP=$(grep 'OTA_IP\|PICO_IP\|OTA_HOST' secrets.h 2>/dev/null | grep -o '"[^"]*"' | tr -d '"' | head -1)

echo "=== LatheStepper build ==="
echo "FQBN: $FQBN"
echo ""

# Compile
echo "Compiling..."
"$ARDUINO_CLI" compile --fqbn "$FQBN" "$SKETCH" || exit 1

case "${1:-}" in
  usb)
    PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
    if [ -z "$PORT" ]; then
      echo "ERROR: No USB serial port found — is the Pico connected?"
      exit 1
    fi
    echo "Uploading via USB on $PORT..."
    "$ARDUINO_CLI" upload -p "$PORT" --fqbn "$FQBN" "$SKETCH"
    ;;
  wifi)
    if [ -z "$PICO_IP" ]; then
      echo -n "Enter Pico IP address: "
      read PICO_IP
    fi
    echo "Uploading via WiFi OTA to $PICO_IP..."
    "$ARDUINO_CLI" upload --port "$PICO_IP" --fqbn "$FQBN" "$SKETCH"
    ;;
  *)
    echo "Compile OK. Run with 'usb' or 'wifi' to upload."
    ;;
esac
