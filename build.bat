@echo off
setlocal
set "BIN=%~dp0tools\llvm-mingw-20261104-ucrt-x86_64\bin"
set "CC=%BIN%\i686-w64-mingw32-clang.exe"
set "RC=%BIN%\i686-w64-mingw32-windres.exe"
set "SRC=%~dp0src"
if not exist "%~dp0bin" mkdir "%~dp0bin"
"%CC%" -O2 -Wall -Wextra -static -shared -o "%~dp0bin\louke.dll" "%SRC%\core.c" "%SRC%\d3d9.c" "%SRC%\games.c" "%SRC%\game_kof13.c" "%SRC%\savestate.c" "%SRC%\comobj.c" "%SRC%\dsound.c" "%SRC%\handles.c" "%SRC%\avdump.c" -luser32 -lole32 -lmfuuid || exit /b 1
pushd "%SRC%"
"%RC%" app.rc -O coff -o "%~dp0bin\app.res.o" || (popd & exit /b 1)
popd
"%CC%" -O2 -Wall -Wextra -static -municode -mwindows -o "%~dp0bin\louke.exe" "%SRC%\gui.c" "%~dp0bin\app.res.o" -lcomctl32 -lcomdlg32 || exit /b 1
del "%~dp0bin\app.res.o"
"%CC%" -O2 -Wall -Wextra -municode -o "%~dp0devtools\testlaunch.exe" "%~dp0devtools\testlaunch.c" || exit /b 1
echo build ok
