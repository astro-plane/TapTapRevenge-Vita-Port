#!/bin/sh
# Double-click this to open the converter window.
cd "$(dirname "$0")" || exit 1
for py in python3 /usr/bin/python3 /usr/local/bin/python3 /opt/homebrew/bin/python3; do
    command -v "$py" >/dev/null 2>&1 && exec "$py" tools/converter_gui.py
done
echo "Python 3 is not installed. Get it from https://www.python.org/downloads/"
read -r _
