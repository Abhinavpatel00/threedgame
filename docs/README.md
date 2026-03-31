# newestvkutil
e
## Windows build (CMake)

Prereqs:
- Visual Studio 2022 or a recent MSVC toolchain
- Vulkan loader + headers (Vulkan SDK or driver package with `vulkan-1.lib`)

Release build:
```
cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64
cmake --build build_win --config Release
```

Debug build:
```
cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64
cmake --build build_win --config Debug
```

Clang build (clang-cl via MSVC generator):
```
cmake -S . -B build_win_clang -G "Visual Studio 17 2022" -A x64 -T ClangCL
cmake --build build_win_clang --config Release
```

Clang + Ninja build:
```
cmake -S . -B build_win_ninja -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build_win_ninja --config Release
```

Notes:
- CMake pulls GLFW automatically via FetchContent in [CMakeLists.txt](CMakeLists.txt).
- If `find_package(Vulkan)` fails, install the Vulkan SDK or ensure the Vulkan loader import library is available.
