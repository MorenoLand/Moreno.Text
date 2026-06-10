# Moreno Text

Moreno Text is a fast, simple, open source editor/IDE inspired by Sublime Text. The goal is a small native binary with strong large-file handling, a clean keyboard-first UI, and a plugin API compatible enough for Sublime-style packages to feel at home.

## Goals

- Stay lightweight and responsive, including very large text files.
- Keep the UI simple, dense, and editor-first.
- Support Sublime-style commands, palettes, settings, syntaxes, color schemes, and Python plugins.
- Be hackable without making plugin authors or contributors jump through weird hoops.

## Status

Moreno Text is early but usable for active development. Core editing, tabs, split groups, syntax highlighting, side bar basics, command palette work, and a Python plugin host are in progress. The plugin API is intentionally being built as a real compatibility layer, not a collection of one-off shims.

## Building

Moreno Text uses CMake and vcpkg.

Windows:

```bat
build.bat
```

Linux/macOS:

```sh
./build.sh
```

Manual CMake:

```sh
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Dependencies

- SDL2
- OpenGL / GLEW
- FreeType
- glm
- nlohmann-json
- Python runtime for plugin hosting

## Compatibility Note

Moreno Text aims to interoperate with user-authored Sublime Text packages and APIs where practical. That compatibility goal is nominative and functional: it helps users move their own workflows and plugins. Moreno Text is not affiliated with, endorsed by, or sponsored by Sublime HQ Pty Ltd.

This project should not copy Sublime Text source code, bundled proprietary assets, private binaries, icons, branding, or paid application materials. API compatibility and independently implemented behavior are the target.

## Versioning

CI passes `MORENO_BUILD_VERSION` into CMake. The default local version is the CMake project version, while GitHub Actions builds use `0.1.<run_number>` so pushed builds get an automatic monotonically increasing version.

## License

Suggested project license: MPL-2.0.

MPL-2.0 keeps changes to Moreno Text source files open while staying friendly to plugin authors, external tools, and contributors who do not want a strong copyleft license touching their separate work.
