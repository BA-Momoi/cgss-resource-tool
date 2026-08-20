@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0\.."

echo [1/2] 配置 CGSS GUI Release 静态版本...
cmake -S . -B build_gui -DCMAKE_BUILD_TYPE=Release -DCGSS_BUILD_GUI=ON -DCGSS_STATIC=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if errorlevel 1 goto error

echo [2/2] 编译 CGSS_GUI.exe...
cmake --build build_gui --target CGSS_GUI -j 8
if errorlevel 1 goto error

echo.
echo 完成: %CD%\build_gui\bin\CGSS_GUI.exe
pause
exit /b 0

:error
echo.
echo 编译失败，请查看上面的第一条错误。
pause
exit /b 1
