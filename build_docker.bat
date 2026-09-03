@echo off
setlocal

set REGION=%1
if "%REGION%"=="" set REGION=us

if not "%REGION%"=="us" if not "%REGION%"=="eu" if not "%REGION%"=="jp" if not "%REGION%"=="sh" if not "%REGION%"=="cn" (
  echo Unknown region: %REGION%
  echo Use one of: us eu jp sh cn   default is us
  pause
  exit /b 1
)

where docker 1>nul 2>nul || (
  echo Docker is not installed.
  echo Please go to https://www.docker.com/get-started and install it first.
  pause
  exit /b 1
)

if not exist baserom.%REGION%.z64 (
  echo A Super Mario 64 ROM is required to extract the assets and build the game.
  echo Put your own dump in this folder, renamed:
  echo  - baserom.us.z64 for the USA version - only working
  pause
  exit /b 1
)

docker build -t sm64ds .
docker run --rm --mount type=bind,source="%cd%",destination=/sm64 sm64ds make VERSION=%REGION% COMPILER=gcc -j4

echo Done. ROM: build/%REGION%_nds/sm64.%REGION%.nds
pause
