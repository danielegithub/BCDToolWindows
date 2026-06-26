@echo off
setlocal EnableDelayedExpansion

echo.
echo  BCDToolWindows - Script di compilazione
echo  =========================================
echo.

:: ── Detect build tools ─────────────────────────────────────────────────────

set BUILD_TOOL=

:: Check for Visual Studio (preferred)
where cl.exe >nul 2>&1
if %errorlevel%==0 (
    set BUILD_TOOL=MSVC
    echo  [*] Trovato: Microsoft Visual C++
    goto :build
)

:: Check for MinGW-w64 (g++)
where g++.exe >nul 2>&1
if %errorlevel%==0 (
    set BUILD_TOOL=MINGW
    echo  [*] Trovato: MinGW-w64 (g++)
    goto :build
)

echo  [!] Errore: nessun compilatore trovato.
echo.
echo  Opzioni:
echo   1. Installa Visual Studio Community (con "Sviluppo desktop C++")
echo      https://visualstudio.microsoft.com/it/downloads/
echo.
echo   2. Installa MSYS2 + MinGW-w64:
echo      https://www.msys2.org/
echo      Poi: pacman -S mingw-w64-x86_64-gcc cmake
echo.
echo   3. Usa CMake con il tuo IDE preferito.
pause
exit /b 1

:build

:: ── CMake build (preferred) ─────────────────────────────────────────────────

where cmake.exe >nul 2>&1
if %errorlevel%==0 (
    echo  [*] Usando CMake...
    echo.

    if not exist "build" mkdir build

    if "%BUILD_TOOL%"=="MSVC" (
        cmake -B build -G "Visual Studio 17 2022" -A x64 .
        if !errorlevel! neq 0 (
            cmake -B build -G "Visual Studio 16 2019" -A x64 .
        )
        cmake --build build --config Release
    ) else (
        cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release .
        cmake --build build
    )

    if !errorlevel!==0 (
        echo.
        echo  [+] Compilazione riuscita!
        echo.
        echo  Eseguibile: build\Release\BCDToolWindows.exe   (MSVC)
        echo          o:  build\BCDToolWindows.exe            (MinGW)
        echo.
        echo  ATTENZIONE: Eseguire sempre come Amministratore!
    ) else (
        echo.
        echo  [!] Compilazione fallita. Controlla gli errori sopra.
    )
    goto :end
)

:: ── Fallback: direct g++ compilation ────────────────────────────────────────

if "%BUILD_TOOL%"=="MINGW" (
    echo  [*] CMake non trovato. Compilazione diretta con g++...
    echo.

    if not exist "bin" mkdir bin

    g++ -std=c++17 -municode -mwindows -O2 -DUNICODE -D_UNICODE ^
        -I src ^
        src\main.cpp ^
        src\privilege.cpp ^
        src\gpt_reader.cpp ^
        src\uefi_vars.cpp ^
        src\esp_scanner.cpp ^
        src\os_detector.cpp ^
        src\boot_editor.cpp ^
        src\backup.cpp ^
        -o bin\BCDToolWindows.exe ^
        -ladvapi32 -lsetupapi -lshell32 -lshlwapi

    if !errorlevel!==0 (
        echo.
        echo  [+] Compilazione riuscita: bin\BCDToolWindows.exe
        echo.
        echo  ATTENZIONE: Eseguire sempre come Amministratore!
    ) else (
        echo.
        echo  [!] Compilazione fallita.
    )
)

:end
echo.
pause
endlocal
