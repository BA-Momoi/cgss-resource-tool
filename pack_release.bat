@echo off
chcp 65001 >nul
cd /d "%~dp0"

set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"
if not exist "%SEVENZIP%" set "SEVENZIP=C:\Program Files (x86)\7-Zip\7z.exe"
if not exist "%SEVENZIP%" (
    echo 7-Zip not found, please install it first.
    goto err
)

echo [1/4] configure Release static ...
cmake -S . -B build_static -DCMAKE_BUILD_TYPE=Release -DCGSS_STATIC=ON
if errorlevel 1 goto err

echo [2/4] build Release CGSS_Script and usm ...
cmake --build build_static --target CGSS_Script usm -j 8
if errorlevel 1 goto err

echo [3/4] assemble complete release directory ...
if not exist "release" mkdir "release"
if not exist "release\CGSS_ResourceTool" mkdir "release\CGSS_ResourceTool"

copy /y build_static\CGSS_Script.exe "release\CGSS_ResourceTool\CGSS_Script.exe" >nul
if errorlevel 1 (
    echo ERROR: cannot replace CGSS_Script.exe. Close the running program and retry.
    goto err
)
copy /y build_static\usm.exe "release\CGSS_ResourceTool\usm.exe" >nul
if errorlevel 1 (
    echo ERROR: cannot copy usm.exe.
    goto err
)
xcopy /E /Y /I build_static\spine_preview "release\CGSS_ResourceTool\spine_preview" >nul
copy /y cgss_apply_textures.py "release\CGSS_ResourceTool\" >nul
copy /y cgss_anim_to_shapekeys.py "release\CGSS_ResourceTool\" >nul
copy /y README.txt "release\CGSS_ResourceTool\README.txt" >nul
if exist ffmpeg.exe copy /y ffmpeg.exe "release\CGSS_ResourceTool\ffmpeg.exe" >nul

rem The release folder may be empty after a fresh checkout. Refill optional
rem runtime assets from the build output when they are available.
if exist "AssetStudio\AssetStudio.CLI.exe" (
    xcopy /E /Y /I "AssetStudio" "release\CGSS_ResourceTool\AssetStudio" >nul
) else if exist "build\AssetStudio\AssetStudio.CLI.exe" (
    xcopy /E /Y /I "build\AssetStudio" "release\CGSS_ResourceTool\AssetStudio" >nul
) else (
    echo WARNING: AssetStudio not found; model/sticker parsing will be unavailable.
)

if exist master.mdb copy /y master.mdb "release\CGSS_ResourceTool\master.mdb" >nul
for %%F in (manifest_*.db) do if exist "%%F" copy /y "%%F" "release\CGSS_ResourceTool\" >nul
if exist "build\acb2wavs.exe" copy /y "build\acb2wavs.exe" "release\CGSS_ResourceTool\acb2wavs.exe" >nul
rem acb2wavs is a managed tool and needs the companion VGAudio/DereTore/
rem SharpDX assemblies beside the executable, not only LZ4.dll.
for %%F in (build\*.dll) do if exist "%%F" copy /y "%%F" "release\CGSS_ResourceTool\" >nul
if exist "build\x64" xcopy /E /Y /I "build\x64" "release\CGSS_ResourceTool\x64" >nul
if exist "build\x86" xcopy /E /Y /I "build\x86" "release\CGSS_ResourceTool\x86" >nul

if not exist "release\CGSS_ResourceTool\master.mdb" echo WARNING: master.mdb not found; data lookup will be unavailable.
if not exist "release\CGSS_ResourceTool\manifest_*.db" echo WARNING: manifest database not found; downloads will be unavailable.

echo [4/4] repack CGSS_ResourceTool.zip ...
del /q release\CGSS_ResourceTool.zip 2>nul
pushd release
"%SEVENZIP%" a -tzip -y CGSS_ResourceTool.zip CGSS_ResourceTool -xr!CGSS_ResourceTool\CGSS_DOWN -xr!CGSS_ResourceTool\AssetStudio_out -xr!CGSS_ResourceTool\spine_preview\__pycache__ -x!CGSS_ResourceTool\AssetStudio\log.txt -x!CGSS_ResourceTool\AssetStudio\log_prev.txt -x!CGSS_ResourceTool\check_update.exe -xr!*.pdb
set "ZIP_RC=%errorlevel%"
popd
if not "%ZIP_RC%"=="0" goto err

echo.
echo ===== DONE =====
dir /-c release\CGSS_ResourceTool.zip | findstr /i "zip"
pause
exit /b 0

:err
echo.
echo ===== BUILD FAILED, see messages above =====
pause
exit /b 1
