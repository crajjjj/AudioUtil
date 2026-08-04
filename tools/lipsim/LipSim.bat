@echo off
rem LipSim launcher: serves lipsim.html over localhost so fuz xWMA audio plays.
rem Double-click me. Needs Python 3.11+ (python.org or Microsoft Store).
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 lipsim_server.py %*
    goto :done
)
where python >nul 2>nul
if %errorlevel%==0 (
    python lipsim_server.py %*
    goto :done
)
echo.
echo   Python 3.11+ is required but was not found.
echo   Install it from https://www.python.org/downloads/ (or the Microsoft
echo   Store), then double-click this file again.
echo.
echo   Without Python you can still open lipsim.html directly in a browser -
echo   everything works except sound for .fuz files.
echo.
pause
:done
endlocal
