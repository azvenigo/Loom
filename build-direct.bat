@echo off
REM ---------------------------------------------------------------------------------------------
REM build-direct.bat - compile Loom without CMake.
REM
REM CMakeLists.txt is the real build definition; this exists because it is a single file that
REM needs no configure step, which is handy for a quick rebuild loop. It mirrors the same warning
REM split: core strict, vendored-header TUs relaxed.
REM
REM Output goes to build-direct\ next to this script. Adjust VSDIR if Visual Studio moves.
REM ---------------------------------------------------------------------------------------------
setlocal
set VSDIR=C:\Program Files\Microsoft Visual Studio\18\Professional
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo Could not find vcvars64.bat under "%VSDIR%" & exit /b 1)

set L=%~dp0
set OUT=%L%build-direct
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"

set STRICT=/std:c++20 /EHsc /permissive- /utf-8 /Wall /WX /nologo /O2 /MT /I "%L%." /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /wd4061 /wd4514 /wd4571 /wd4623 /wd4625 /wd4626 /wd4710 /wd4711 /wd4820 /wd5026 /wd5027 /wd5045 /wd5262
set LOOSE=/std:c++20 /EHsc /permissive- /utf-8 /W3 /nologo /O2 /MT /I "%L%." /I "%L%vendor\asio" /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DASIO_STANDALONE /DCROW_DISABLE_STATIC_DIR /D_WIN32_WINNT=0x0A00

echo [1/4] core (strict)
cl /c %STRICT% "%L%core\JotStore.cpp" "%L%core\Ops.cpp" "%L%core\TagRegistry.cpp" "%L%core\Tokenizer.cpp" "%L%core\LoomTime.cpp"
if errorlevel 1 exit /b 1

echo [2/4] codec, persist, mcp, http (vendored headers, relaxed)
cl /c %LOOSE% "%L%codec\JotJson.cpp" "%L%persist\Journal.cpp" "%L%persist\Importer.cpp" "%L%persist\Snapshot.cpp" "%L%mcp\McpHandler.cpp" "%L%http\HttpServer.cpp" "%L%main.cpp"
if errorlevel 1 exit /b 1

echo [3/4] loom.exe
link /nologo /OUT:loom.exe JotStore.obj Ops.obj TagRegistry.obj Tokenizer.obj LoomTime.obj JotJson.obj Journal.obj Importer.obj Snapshot.obj McpHandler.obj HttpServer.obj main.obj ws2_32.lib mswsock.lib
if errorlevel 1 exit /b 1

echo [4/4] tests and benchmark
set CORELIB=JotStore.obj Ops.obj TagRegistry.obj Tokenizer.obj LoomTime.obj
cl %STRICT% "%L%test\coretest\main.cpp" %CORELIB% /Fe:coretest.exe
if errorlevel 1 exit /b 1
cl %LOOSE% "%L%test\persisttest\main.cpp" %CORELIB% JotJson.obj Journal.obj Importer.obj Snapshot.obj /Fe:persisttest.exe
if errorlevel 1 exit /b 1
cl %LOOSE% "%L%test\mcptest\main.cpp" %CORELIB% JotJson.obj McpHandler.obj /Fe:mcptest.exe
if errorlevel 1 exit /b 1
cl %STRICT% "%L%bench\loombench\main.cpp" %CORELIB% /Fe:loombench.exe
if errorlevel 1 exit /b 1

echo.
echo Built in %OUT%
endlocal
