mkdir cmake
cd cmake
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --target Luau.Repl.CLI Luau.Analyze.CLI Luau.Compile.CLI Luau.Ast.CLI --config RelWithDebInfo -j 2
cp RelWithDebInfo\luau.exe C:\Lua\bin\luau-src\luau.exe
cp RelWithDebInfo\luau-analyze.exe C:\Lua\bin\luau-src\luau-analyze.exe
cp RelWithDebInfo\luau-ast.exe C:\Lua\bin\luau-src\luau-ast.exe
cd ..