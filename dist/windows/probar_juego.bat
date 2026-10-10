@echo off
rem WIISP para Windows: arrastra la ISO (o CSO) del juego encima de este archivo.
rem Deja wiisp.log y capturas .bmp en esta carpeta. Ctrl+C para parar antes
rem (el registro y la captura final se guardan igual).
cd /d "%~dp0"
if "%~1"=="" (
	echo Arrastra la ISO del juego encima de probar_juego.bat
	pause
	exit /b 1
)
del /q captura_*.bmp pantalla_final.bmp 2>nul
echo Ejecutando "%~nx1" durante 3 minutos de juego como maximo.
echo Puede tardar varios minutos. Ctrl+C para parar antes.
echo Con esta ventana delante, W A S D mueven el stick analogico.
echo.
wiisp-cli.exe --run --quiet --frames 10800 --capturas 150 --log wiisp.log --screenshot pantalla_final.bmp "%~1"
echo.
echo Terminado. Manda wiisp.log y las capturas .bmp de esta carpeta.
pause
