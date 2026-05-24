# VLSI Topology Decomposition

C++ проект для иерархической декомпозиции топологий VLSI.
Использует `gdstk` для парсинга GDSII, `qhull` для вычислительной геометрии и `zlib` для работы со сжатыми данными.

## 📦 Зависимости
- `CMake ≥ 3.15`
- `C++17` совместимый компилятор (MSVC, GCC, Clang)
- Исходные коды внешних библиотек (размещаются в `dependencies/`):
  - [`zlib`](https://github.com/madler/zlib)
  - [`qhull`](https://github.com/qhull/qhull)
  - [`gdstk`](https://github.com/heitzmann/gdstk)

## 🛠️ Сборка

### Windows
```cmd
.\build.bat