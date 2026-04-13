@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
call D:\jam\venv\Scripts\activate.bat

for /f "delims=" %%i in ('rocm-sdk path --root') do set LLVM_BIN=%%i\lib\llvm\bin

echo LLVM_BIN=%LLVM_BIN%

rmdir /s /q D:\jam\doomgeneric\build 2>nul

echo Configuring...
cmake -G Ninja -S D:\jam\doomgeneric -B D:\jam\doomgeneric\build ^
  -DCMAKE_C_COMPILER="%LLVM_BIN%\clang.exe" ^
  -DCMAKE_CXX_COMPILER="%LLVM_BIN%\clang++.exe" ^
  -DCMAKE_LINKER="%LLVM_BIN%\lld-link.exe" ^
  -DCMAKE_BUILD_TYPE=Release

if errorlevel 1 (
  echo Configure FAILED
  exit /b 1
)

echo Building...
cmake --build D:\jam\doomgeneric\build 2>&1

if errorlevel 1 (
  echo Build FAILED
  exit /b 1
)

echo Build SUCCESS
