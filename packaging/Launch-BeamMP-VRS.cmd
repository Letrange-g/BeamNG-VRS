@echo off
rem Starts BeamMP (multiplayer) with BeamNG-VRS. Add -BeamMPLauncher "D:\path\BeamMP-Launcher.exe" if BeamMP is not in its default folder.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0BeamNG-VRS\Launch-BeamNG-VRS.ps1" -BeamMP
if errorlevel 1 pause
