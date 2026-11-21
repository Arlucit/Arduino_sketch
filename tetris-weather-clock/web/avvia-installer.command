#!/bin/bash
# Avvia un piccolo server locale e apre la pagina di installazione (macOS / Linux, serve Python 3)
cd "$(dirname "$0")"
if ! command -v python3 >/dev/null 2>&1; then echo "Python 3 non trovato."; exit 1; fi
( sleep 1; (open "http://localhost:8000/" || xdg-open "http://localhost:8000/") >/dev/null 2>&1 ) &
echo "Server avviato su http://localhost:8000  -  premi Ctrl+C per fermarlo."
python3 -m http.server 8000 --bind 127.0.0.1
