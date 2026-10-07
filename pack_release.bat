@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"
if not exist "%SEVENZIP%" set "SEVENZIP=C:\Program Files (x86)\7-Zip\7z.exe"
if not exist "%SEVENZIP%" (
    echo 7-Zip not found, please install it first.
    goto err
)

echo [1/6] configure Release static ...
cmake -S . -B build_static -DCMAKE_BUILD_TYPE=Release -DCGSS_STATIC=ON
if errorlevel 1 goto err

echo [2/6] build Release CGSS_Script and usm ...
cmake --build build_static --target CGSS_Script usm -j 8
if errorlevel 1 goto err

echo [3/6] validate required release assets ...
if not exist "build_static\CGSS_Script.exe" goto missing_main
if not exist "build_static\usm.exe" goto missing_usm
if not exist "spine_preview\preview.html" goto missing_preview
if not exist "cgss_apply_textures.py" goto missing_blender_scripts
if not exist "cgss_anim_to_shapekeys.py" goto missing_blender_scripts
if not exist "README.txt" goto missing_readme
if not exist "master.mdb" goto missing_master
if not exist "stage_live_map.csv" goto missing_stage_map
if not exist "ffmpeg.exe" goto missing_ffmpeg
if not exist "build\acb2wavs.exe" goto missing_audio_tool
if not exist "build\x64" goto missing_audio_runtime
if not exist "build\x86" goto missing_audio_runtime

set "HAVE_MANIFEST="
for %%F in (manifest_*.db) do if exist "%%F" set "HAVE_MANIFEST=1"
if not defined HAVE_MANIFEST goto missing_manifest

set "HAVE_DLL="
for %%F in (build\*.dll) do if exist "%%F" set "HAVE_DLL=1"
if not defined HAVE_DLL goto missing_audio_dlls

set "ASSETSTUDIO_SOURCE="
if exist "AssetStudio\AssetStudio.CLI.exe" set "ASSETSTUDIO_SOURCE=AssetStudio"
if not defined ASSETSTUDIO_SOURCE if exist "build\AssetStudio\AssetStudio.CLI.exe" set "ASSETSTUDIO_SOURCE=build\AssetStudio"
if not defined ASSETSTUDIO_SOURCE goto missing_assetstudio

set "STAGE="
:new_stage
set "STAGE=%TEMP%\CGSS_ResourceTool_%RANDOM%_%RANDOM%"
if exist "%STAGE%" goto new_stage
set "PACKAGE=%STAGE%\CGSS_ResourceTool"
mkdir "%PACKAGE%"
if errorlevel 1 goto err

echo [4/6] assemble a clean, portable release ...
copy /y "build_static\CGSS_Script.exe" "%PACKAGE%\CGSS_Script.exe" >nul
if errorlevel 1 goto err
copy /y "build_static\usm.exe" "%PACKAGE%\usm.exe" >nul
if errorlevel 1 goto err
xcopy /E /Y /I "build_static\spine_preview" "%PACKAGE%\spine_preview" >nul
if errorlevel 1 goto err
copy /y "cgss_apply_textures.py" "%PACKAGE%\" >nul
if errorlevel 1 goto err
copy /y "cgss_anim_to_shapekeys.py" "%PACKAGE%\" >nul
if errorlevel 1 goto err
copy /y "README.txt" "%PACKAGE%\README.txt" >nul
if errorlevel 1 goto err
copy /y "ffmpeg.exe" "%PACKAGE%\ffmpeg.exe" >nul
if errorlevel 1 goto err
copy /y "master.mdb" "%PACKAGE%\master.mdb" >nul
if errorlevel 1 goto err
copy /y "stage_live_map.csv" "%PACKAGE%\stage_live_map.csv" >nul
if errorlevel 1 goto err
copy /y manifest_*.db "%PACKAGE%\" >nul
if errorlevel 1 goto err
if exist "master.mdb.sync" copy /y "master.mdb.sync" "%PACKAGE%\master.mdb.sync" >nul
xcopy /E /Y /I "%ASSETSTUDIO_SOURCE%" "%PACKAGE%\AssetStudio" >nul
if errorlevel 1 goto err
copy /y "build\acb2wavs.exe" "%PACKAGE%\acb2wavs.exe" >nul
if errorlevel 1 goto err
for %%F in (build\*.dll) do if exist "%%F" copy /y "%%F" "%PACKAGE%\" >nul
xcopy /E /Y /I "build\x64" "%PACKAGE%\x64" >nul
if errorlevel 1 goto err
xcopy /E /Y /I "build\x86" "%PACKAGE%\x86" >nul
if errorlevel 1 goto err

powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0bundle_dotnet_runtime.ps1" -Destination "%PACKAGE%\dotnet"
if errorlevel 1 goto err

if not exist "release" mkdir "release"
if errorlevel 1 goto err
pushd "%STAGE%"
"%SEVENZIP%" a -tzip -y "CGSS_ResourceTool.zip" "CGSS_ResourceTool" -x!CGSS_ResourceTool\AssetStudio\log.txt -x!CGSS_ResourceTool\AssetStudio\log_prev.txt -xr!*.pdb
set "ZIP_RC=%errorlevel%"
popd
if not "%ZIP_RC%"=="0" goto err

echo [5/6] replace the release archive after it is complete ...
move /Y "%STAGE%\CGSS_ResourceTool.zip" "%CD%\release\CGSS_ResourceTool.zip" >nul
if errorlevel 1 goto err

echo [6/6] refresh the unpacked release files ...
pushd "release"
"%SEVENZIP%" x -y "CGSS_ResourceTool.zip" >nul
set "EXTRACT_RC=%errorlevel%"
popd
if not "%EXTRACT_RC%"=="0" echo WARNING: the ZIP is ready, but the unpacked release folder could not be refreshed.

rmdir /S /Q "%STAGE%"
set "STAGE="

echo.
echo ===== DONE =====
dir /-c "%CD%\release\CGSS_ResourceTool.zip" | findstr /i "zip"
pause
exit /b 0

:missing_main
echo ERROR: CGSS_Script.exe was not produced.
goto err
:missing_usm
echo ERROR: usm.exe was not produced.
goto err
:missing_preview
echo ERROR: spine_preview\preview.html is missing.
goto err
:missing_blender_scripts
echo ERROR: required Blender scripts are missing.
goto err
:missing_readme
echo ERROR: README.txt is missing.
goto err
:missing_master
echo ERROR: master.mdb is required for a full release.
goto err
:missing_stage_map
echo ERROR: stage_live_map.csv is required for exact 3D stage lookup.
goto err
:missing_manifest
echo ERROR: manifest_*.db is required for downloads.
goto err
:missing_assetstudio
echo ERROR: AssetStudio.CLI.exe is required for model and Spine extraction.
goto err
:missing_ffmpeg
echo ERROR: ffmpeg.exe is required for video conversion.
goto err
:missing_audio_tool
echo ERROR: build\acb2wavs.exe is required for audio decoding.
goto err
:missing_audio_runtime
echo ERROR: build\x64 and build\x86 are required for audio decoding.
goto err
:missing_audio_dlls
echo ERROR: build\*.dll dependencies are required for audio decoding.
goto err

:err
if defined STAGE if exist "%STAGE%" rmdir /S /Q "%STAGE%"
echo.
echo ===== BUILD FAILED, see messages above =====
pause
exit /b 1
