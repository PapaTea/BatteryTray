@echo off
rem BatteryTray — сборка одним файлом
setlocal
where cl >nul 2>nul || (
  echo [!] Не найден cl.exe. Запусти из "x64 Native Tools Command Prompt for VS 2022"
  exit /b 1
)
if not exist obj mkdir obj
cl /nologo /std:c++17 /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE /Fo:obj\ /Fd:obj\vc.pdb ^
   src\main.cpp src\chip.cpp src\config.cpp src\hid_providers.cpp src\ble_providers.cpp src\xinput_provider.cpp ^
   /link windowsapp.lib shell32.lib user32.lib gdi32.lib gdiplus.lib setupapi.lib hid.lib ole32.lib xinput.lib comctl32.lib ^
   /SUBSYSTEM:WINDOWS /out:BatteryTray.exe
if errorlevel 1 exit /b 1
echo.
echo [ok] BatteryTray.exe собран
