# Moreno Text

A text editor.

## Building

```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Or just run `build.bat` on Windows / `build.sh` on Linux/macOS.

Requires [vcpkg](https://vcpkg.io/) for dependency management.

## Dependencies

- SDL2
- FreeType
- GLEW
- glm
- nlohmann-json

## License

GPL v3
