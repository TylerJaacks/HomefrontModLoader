@echo off
rem Build HomefrontModLoader (32-bit) as HomefrontModLoader\build\msimg32.dll. HomefrontSDK\Build.ps1 runs this and
rem installs the result.
setlocal
set "HERE=%~dp0"
set "OUT=%HERE%build"
if not exist "%OUT%" mkdir "%OUT%"

rem vswhere's path contains "(x86)", which breaks a for /f loop, so read its output via a file.
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * ^
   -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%OUT%\vs_path.txt"
set /p VS=<"%OUT%\vs_path.txt"
if "%VS%"=="" (echo No Visual Studio with x86 C++ tools found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>nul || exit /b 1

cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /LD /bigobj "%HERE%loader.cpp" "%HERE%vac.cpp" "%HERE%native_api.cpp" ^
   advapi32.lib /Fo"%OUT%\\" /link /OUT:"%OUT%\msimg32.dll" /IMPLIB:"%OUT%\msimg32.lib" || exit /b 1
echo built %OUT%\msimg32.dll
