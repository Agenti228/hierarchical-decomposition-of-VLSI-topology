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
```

## 🚀 Использование

### Windows
```powershell
.\build\Release\vlsi_topology.exe
```

После запуска программа запросит параметры в интерактивном режиме:

```
Input absolute path to a GDS file: C:\<путь_к_файлу>\<название_файла>.gds
Output directory [./output_patterns]: C:\<путь_к_выходной_папке>\output
Search window width in um [5.0]: 6.5
Search window height in um [5.0]: 3.0
Max search iterations [100]: 200
```

| Параметр | Описание | Пример |
|---|---|---|
| Input absolute path to a GDS file | Абсолютный путь к входному GDSII файлу | `C:\data\topology.gds` |
| Output directory | Папка для выходных файлов (создаётся автоматически) | `C:\data\output` |
| Search window width in um | Ширина окна поиска паттернов в микронах | `3.0` |
| Search window height in um | Высота окна поиска паттернов в микронах | `8.0` |
| Max search iterations | Максимальное число итераций расширения паттернов | `50` |

Если нажать Enter (без ввода), будет использовано значение, указанное в квадратных скобках.

---

## 📂 Структура выходных данных

После завершения в указанной папке появятся:

```
output_patterns/
  primitives/
    primitive_1.gds  <- геометрия первого примитива
    primitive_2.gds  <- геометрия второго примитива
    ...
    primitive_N.gds
  pattern_1.gds      <- геометрия первого паттерна
  pattern_2.gds      <- геометрия второго паттерна
  ...
  pattern_N.gds
  pattern_stats.txt  <- статистика поиска паттренов 
  patterns.txt       <- координаты размещений
```

### Формат patterns.txt

```
pattern_1.gds <-> (x1,y1), (x2,y2), ..., (xn,yn)
pattern_2.gds <-> (x1,y1), (x2,y2), ..., (xk,yk)
```

Координаты указаны в микронах и соответствуют **левому нижнему углу** каждого размещения паттерна на исходной топологии.

---
