call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd C:\Galaxy3DVision\Projects\Sunbridge
cmake -B build/x64-Release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build/x64-Release --config Release > build_log.txt 2>&1
copy /Y build\x64-Release\Release\sunshine_bridge.dll C:\Galaxy3DVision\Projects\out\sunshine_bridge.dll
