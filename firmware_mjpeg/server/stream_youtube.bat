@echo off
setlocal
rem MJPEG-стрим YouTube для MiniScreen (нужны yt-dlp и ffmpeg)
rem Использование: stream_youtube.bat ["https://youtube.com/watch?v=..."]
rem Без аргумента играет видео по умолчанию (см. DEFAULT_URL ниже)

set URL=%~1
if "%URL%"=="" set URL=https://www.youtube.com/watch?v=cWlE4VrhRAg

echo Получаю прямую ссылку на видео...
set VURL=
for /f "usebackq delims=" %%u in (`yt-dlp -f "best[height<=480]/best" -g "%URL%"`) do set VURL=%%u
if "%VURL%"=="" (
    echo ОШИБКА: не удалось получить ссылку. Проверь yt-dlp и доступность видео.
    pause
    exit /b 1
)

echo.
echo Стрим на http://0.0.0.0:8090/   (Ctrl+C — остановить)
echo ESP подключать на http://IP_ЭТОГО_ПК:8090/
echo.

:loop
ffmpeg -hide_banner -re -i "%VURL%" ^
  -vf "scale=240:240:force_original_aspect_ratio=increase,crop=240:240,fps=20" ^
  -c:v mjpeg -q:v 7 -an ^
  -listen 1 -f mpjpeg http://0.0.0.0:8090/
echo Перезапуск (ссылка могла протухнуть — обновляю)...
for /f "usebackq delims=" %%u in (`yt-dlp -f "best[height<=480]/best" -g "%URL%"`) do set VURL=%%u
goto loop
