@echo off
rem Compile PhysicalGauge and upload it to the ESP32 over Wi-Fi (OTA).
rem Double-click to run. Save the sketch in the Arduino IDE first.
rem
rem ESP32_IP: the address printed at boot / by the "wifi" serial command.
rem If it changes, reserve it in your router (DHCP reservation) or edit it here.
rem OTA_PASS must match OTA_PASS in the code currently running on the ESP32.

set ESP32_IP=192.168.86.59
set OTA_PASS=kegerator-ota
set OTA_USER=admin

set CLI="C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
set SKETCH="%~dp0PhysicalGauge"
rem Own build folder, so it doesn't collide with the IDE's (which it keeps locked).
set BUILD=%TEMP%\PhysicalGauge-ota

rem espota.exe ships with the ESP32 core; pick it up from whichever version is installed.
set ESPOTA=
for /d %%D in ("%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\*") do (
  if exist "%%D\tools\espota.exe" set ESPOTA=%%D\tools\espota.exe
)
if "%ESPOTA%"=="" (
  echo Could not find espota.exe in the ESP32 core.
  goto failed
)

echo Compiling...
%CLI% compile --fqbn esp32:esp32:esp32 --build-path "%BUILD%" %SKETCH%
if errorlevel 1 goto failed

rem 1st choice: espota straight to the IP (no network discovery needed). The ESP32
rem connects back to this PC, so it needs the espota firewall permission.
echo.
echo Uploading to %ESP32_IP% with espota ...
"%ESPOTA%" -i %ESP32_IP% -a %OTA_PASS% -r -f "%BUILD%\PhysicalGauge.ino.bin"
if not errorlevel 1 goto done

rem Fallback: post the .bin to the ESP32's /update web page. Outgoing request only,
rem so no firewall involvement. (The Origin header is required by the update page.)
echo.
echo espota failed - trying the /update web page instead ...
curl -sS -u %OTA_USER%:%OTA_PASS% -H "Origin: http://%ESP32_IP%" -F "firmware=@%BUILD%\PhysicalGauge.ino.bin" http://%ESP32_IP%/update | findstr /C:"Update Success"
if errorlevel 1 goto failed

:done
echo.
echo Done. The ESP32 is rebooting with the new code.
pause
exit /b 0

:failed
echo.
echo FAILED - see messages above.
pause
exit /b 1
