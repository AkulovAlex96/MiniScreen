@echo off
rem MJPEG-стрим видеофайла для MiniScreen (крутится в цикле)
rem Использование: stream.bat video.mp4 [fps] [quality 2-15, меньше=лучше]

set FILE=%1
if "%FILE%"=="" set FILE=video.mp4
set FPS=%2
if "%FPS%"=="" set FPS=20
set Q=%3
if "%Q%"=="" set Q=7

echo Стрим %FILE% на http://0.0.0.0:8090/  (%FPS% fps, q=%Q%)
echo ESP подключать на http://IP_ЭТОГО_ПК:8090/
echo.

:loop
ffmpeg -re -stream_loop -1 -i "%FILE%" ^
  -vf "scale=240:240:force_original_aspect_ratio=increase,crop=240:240,fps=%FPS%" ^
  -c:v mjpeg -q:v %Q% -an ^
  -listen 1 -f mpjpeg http://0.0.0.0:8090/
echo Клиент отключился, жду следующего...
goto loop
