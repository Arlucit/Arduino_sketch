@echo off
rem Avvia un piccolo server locale e apre la pagina di installazione (serve Python 3)
cd /d "%~dp0"
where py >nul 2>nul && (set PY=py -3) || (where python >nul 2>nul && (set PY=python) || (echo Python 3 non trovato. Installalo da https://www.python.org/downloads/ oppure usa la pagina su GitHub Pages. & pause & exit /b 1))
start "" "http://localhost:8000/"
echo Server avviato su http://localhost:8000  -  chiudi questa finestra per fermarlo.
%PY% -m http.server 8000 --bind 127.0.0.1
