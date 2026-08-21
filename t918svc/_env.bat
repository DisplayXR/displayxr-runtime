@echo off
REM #918 Phase 2b PR 3 — shared paths for the dev-service test wrappers.
REM Sourced by every other .bat in this folder; not meant to be run alone.
set "DXR_WT=%~dp0.."
set "DXR_PKG=%DXR_WT%\_package"
set "DXR_APPS=%DXR_WT%\test_apps\build\bin"
set "DXR_JSON=%DXR_PKG%\bin\DisplayXR_win64.json"
set "DXR_INSTALLED=C:\Program Files\DisplayXR\Runtime\displayxr-service.exe"
