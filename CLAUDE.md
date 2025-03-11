# CLAUDE.md - PlatformIO ESP32 Firmware Project Guide

## Build Commands
- Build project: `pio run`
- Upload firmware: `pio run -t upload`
- Monitor serial output: `pio run -t monitor`
- Clean build files: `pio run -t clean`
- Run specific test: `pio test -e T-SIM7600X -f <test_name>`

## Code Style Guidelines
- Use camelCase for variables and methods, PascalCase for classes
- Include function documentation using inline comments
- Keep methods concise (<50 lines where possible)
- Prefer strong typing and reference parameters for class objects
- Errors should be handled with clear error messages and graceful failure
- Headers have guard defines: `#ifndef FILE_H`, `#define FILE_H`, `#endif`
- C++11 features allowed: auto, range loops, smart pointers, std::vector

## Project Structure
- `src/`: Implementation files (.cpp)
- `include/`: Header files (.h)
- `lib/`: External libraries
- `test/`: Unit tests
- `platformio.ini`: Project configuration

## Important Constants
- Hardware pins defined in utilities.h
- MQTT topics in constants.h
- When adding new features, follow existing error handling patterns