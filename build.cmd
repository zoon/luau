REM mkdir cmake
cd cmake
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --target Luau.Repl.CLI --config RelWithDebInfo
cmake --build . --target Luau.Analyze.CLI --config RelWithDebInfo
cp RelWithDebInfo\luau.exe C:\Lua\bin\luau-src\luau.exe
cp RelWithDebInfo\luau-analyze.exe C:\Lua\bin\luau-src\luau-analyze.exe
cd ..