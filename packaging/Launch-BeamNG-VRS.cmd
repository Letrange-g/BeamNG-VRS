@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0BeamNG-VRS\Launch-BeamNG-VRS.ps1"
if errorlevel 1 pause
