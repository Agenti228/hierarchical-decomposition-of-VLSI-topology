#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <gdstk/gdstk.hpp>

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::unordered_map;
using std::vector;

namespace fs = std::filesystem;

struct Config
{
    string input_file = "";
    string output_dir = "./output_patterns";
    double win_w_um = 5.0;
    double win_h_um = 5.0;
    int max_iter = 100;
};

template <typename T>
static void hash_combine(uint64_t &seed, const T &value)
{
    std::hash<T> hasher;
    seed ^= hasher(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

struct IntVec2
{
    int64_t x = 0;
    int64_t y = 0;

    bool operator==(const IntVec2 &other) const
    {
        return x == other.x && y == other.y;
    }

    bool operator!=(const IntVec2 &other) const
    {
        return !(*this == other);
    }

    bool operator<(const IntVec2 &other) const
    {
        if (x != other.x)
            return x < other.x;
        return y < other.y;
    }

    IntVec2 operator-(const IntVec2 &other) const
    {
        return {x - other.x, y - other.y};
    }

    IntVec2 operator+(const IntVec2 &other) const
    {
        return {x + other.x, y + other.y};
    }
};

struct Bbox
{
    int64_t min_x = 0, max_y = 0;
    int64_t min_y = 0, max_x = 0;

    bool operator==(const Bbox &other) const
    {
        return min_x == other.min_x &&
               min_y == other.min_y;
    }
};

static int64_t to_grid(double val, double grid_step)
{
    return (int64_t)std::round(val / grid_step);
}

static double from_grid(int64_t val, double grid_step)
{
    return (double)val * grid_step;
}

template <typename T>
static int get_minimal_point_idx(const T &non_canonical_points)
{
    int start_index = 0;

    for (int i = 1; i < non_canonical_points.count; i++)
    {
        if (non_canonical_points[i] < non_canonical_points[start_index])
        {
            start_index = i;
        }
    }

    return start_index;
}

struct Primitive
{
    uint64_t canonical_hash = 0;
    vector<IntVec2> canonical_points;
    size_t layer_id = 0;
    IntVec2 bbox_min = {};

    Primitive() = default;

    Primitive(const gdstk::Array<gdstk::Vec2> non_canonical_points, const size_t layer, double grid_step)
    {
        layer_id = layer;

        int n = (int)non_canonical_points.count;
        int start = get_minimal_point_idx(non_canonical_points);

        IntVec2 origin = {
            to_grid(non_canonical_points[start].x, grid_step),
            to_grid(non_canonical_points[start].y, grid_step)};

        for (int i = 0; i < n; ++i)
        {
            int j = (start + i) % n;
            IntVec2 p = {
                to_grid(non_canonical_points[j].x, grid_step) - origin.x,
                to_grid(non_canonical_points[j].y, grid_step) - origin.y};
            canonical_points.push_back(p);
        }

        bbox_min = canonical_points[0];
        for (const IntVec2 &cp : canonical_points)
        {
            bbox_min.x = std::min(bbox_min.x, cp.x);
            bbox_min.y = std::min(bbox_min.y, cp.y);
        }

        canonical_hash = 0;
        hash_combine(canonical_hash, layer_id);
        for (const IntVec2 &cp : canonical_points)
        {
            hash_combine(canonical_hash, cp.x);
            hash_combine(canonical_hash, cp.y);
        }
    }
};

struct Pattern
{
    struct PatternElement
    {
        IntVec2 position_in_pattern = {};
        size_t pattern_idx = SIZE_MAX;

        bool operator==(const PatternElement &other) const
        {
            return position_in_pattern == other.position_in_pattern && pattern_idx == other.pattern_idx;
        }

        bool operator<(const PatternElement &other) const
        {
            if (position_in_pattern != other.position_in_pattern)
            {
                return position_in_pattern < other.position_in_pattern;
            }

            return pattern_idx < other.pattern_idx;
        }
    };

    uint64_t canonical_hash = 0;
    vector<PatternElement> pattern_elements;
    Bbox bbox;
    bool is_leaf = false;
};

static uint64_t compute_pattern_hash(const Pattern &pattern)
{
    uint64_t hash = 0;

    for (const Pattern::PatternElement &element : pattern.pattern_elements)
    {
        hash_combine(hash, element.position_in_pattern.x);
        hash_combine(hash, element.position_in_pattern.y);
        hash_combine(hash, element.pattern_idx);
    }

    return hash;
}

template <typename T>
struct Registry
{
    unordered_map<uint64_t, size_t> canonical_key_to_entry_idx;
    vector<T> unique_entries;

    size_t get_or_insert(const uint64_t &key, const T &obj)
    {
        auto [it, inserted] = canonical_key_to_entry_idx.emplace(key, unique_entries.size());

        if (inserted)
        {
            unique_entries.push_back(obj);
        }

        return it->second;
    }

    const T &operator[](size_t index) const
    {
        return unique_entries[index];
    }

    size_t size() const
    {
        return unique_entries.size();
    }

    bool contains(const uint64_t &key) const
    {
        return canonical_key_to_entry_idx.count(key) > 0;
    }

    int index_of(uint64_t key) const
    {
        return canonical_key_to_entry_idx.at(key);
    }
};

struct Shape
{
    IntVec2 position_on_canvas = {};
    size_t pattern_idx = 0;
    Bbox bbox;
    bool absorbed = false;

    void compute_bbox(const Primitive &primitive)
    {
        bbox.min_x = primitive.canonical_points[0].x + position_on_canvas.x;
        bbox.min_y = primitive.canonical_points[0].y + position_on_canvas.y;

        bbox.max_x = bbox.min_x;
        bbox.max_y = bbox.min_y;

        for (const IntVec2 &point : primitive.canonical_points)
        {
            int64_t x = point.x + position_on_canvas.x;
            int64_t y = point.y + position_on_canvas.y;

            bbox.min_x = std::min(bbox.min_x, x);
            bbox.min_y = std::min(bbox.min_y, y);
            bbox.max_x = std::max(bbox.max_x, x);
            bbox.max_y = std::max(bbox.max_y, y);
        }
    }

    bool inside_window(int64_t x, int64_t y, int64_t width, int64_t height) const
    {
        return bbox.min_x >= x && bbox.min_y >= y &&
               bbox.max_x <= x + width && bbox.max_y <= y + height;
    }
};

struct GDSContext
{
    Registry<Primitive> primitive_registry;
    Registry<Pattern> pattern_registry;
    vector<Shape> shapes;
    double grid_step = 0.001;
};

struct NeighborKey
{
    int64_t offset_x = 0;
    int64_t offset_y = 0;
    size_t pattern_idx_a = 0;
    size_t pattern_idx_b = 0;

    bool operator==(const NeighborKey &other) const
    {
        return offset_x == other.offset_x &&
               offset_y == other.offset_y &&
               pattern_idx_a == other.pattern_idx_a &&
               pattern_idx_b == other.pattern_idx_b;
    }
};

struct NeighborKeyHash
{
    size_t operator()(const NeighborKey &key) const
    {
        uint64_t hash = 0;
        hash_combine(hash, key.offset_x);
        hash_combine(hash, key.offset_y);
        hash_combine(hash, key.pattern_idx_a);
        hash_combine(hash, key.pattern_idx_b);
        return hash;
    }
};

struct PotentialPattern
{
    size_t shape_idx = 0;
    size_t neighbor_idx = 0;
};

static unordered_map<NeighborKey, vector<PotentialPattern>, NeighborKeyHash> rigister_all_shape_pairs_as_patterns(const GDSContext &gds_context, int64_t win_w, int64_t win_h)
{
    unordered_map<NeighborKey, vector<PotentialPattern>, NeighborKeyHash> pattern_pairs;

    if (gds_context.shapes.size() < 2)
    {
        return {};
    }

    for (size_t shape_idx = 0; shape_idx < gds_context.shapes.size() - 1; shape_idx++)
    {
        const Shape &shape = gds_context.shapes[shape_idx];

        if (shape.absorbed)
        {
            continue;
        }

        for (size_t neighbor_idx = shape_idx + 1; neighbor_idx < gds_context.shapes.size(); neighbor_idx++)
        {
            const Shape &neighbor = gds_context.shapes[neighbor_idx];

            if (neighbor.absorbed)
            {
                continue;
            }

            const Pattern &pat_shape = gds_context.pattern_registry[shape.pattern_idx];
            const Pattern &pat_neighbor = gds_context.pattern_registry[neighbor.pattern_idx];

            int64_t anchor_x = shape.bbox.min_x + pat_shape.bbox.min_x;
            int64_t anchor_y = shape.bbox.min_y + pat_shape.bbox.min_y;

            // Абсолютный bbox shape
            int64_t shape_max_x = shape.bbox.min_x + pat_shape.bbox.max_x;
            int64_t shape_max_y = shape.bbox.min_y + pat_shape.bbox.max_y;

            // Абсолютный bbox neighbor
            int64_t neighbor_min_x = neighbor.bbox.min_x + pat_neighbor.bbox.min_x;
            int64_t neighbor_min_y = neighbor.bbox.min_y + pat_neighbor.bbox.min_y;
            int64_t neighbor_max_x = neighbor.bbox.min_x + pat_neighbor.bbox.max_x;
            int64_t neighbor_max_y = neighbor.bbox.min_y + pat_neighbor.bbox.max_y;

            // Общий bbox пары
            int64_t pair_min_x = std::min(anchor_x, neighbor_min_x);
            int64_t pair_min_y = std::min(anchor_y, neighbor_min_y);
            int64_t pair_max_x = std::max(shape_max_x, neighbor_max_x);
            int64_t pair_max_y = std::max(shape_max_y, neighbor_max_y);

            // Проверяем что пара помещается в окно
            if (pair_max_x - pair_min_x > win_w)
            {
                continue;
            }
            if (pair_max_y - pair_min_y > win_h)
            {
                continue;
            }

            NeighborKey key;
            key.offset_x = neighbor.bbox.min_x - shape.bbox.min_x;
            key.offset_y = neighbor.bbox.min_y - shape.bbox.min_y;
            key.pattern_idx_a = shape.pattern_idx;
            key.pattern_idx_b = neighbor.pattern_idx;

            PotentialPattern potential_pattern;
            potential_pattern.shape_idx = shape_idx;
            potential_pattern.neighbor_idx = neighbor_idx;

            pattern_pairs[key].push_back(potential_pattern);
        }
    }

    return pattern_pairs;
}

static unordered_map<NeighborKey, vector<PotentialPattern>, NeighborKeyHash> register_all_shape_pairs_as_patterns_fast(const GDSContext &ctx, int64_t win_w, int64_t win_h)
{
    unordered_map<NeighborKey, vector<PotentialPattern>, NeighborKeyHash> pattern_pairs;

    // Строим отсортированный индекс активных фигур по bbox.min_x
    vector<size_t> sorted_indices;
    sorted_indices.reserve(ctx.shapes.size());
    for (size_t i = 0; i < ctx.shapes.size(); ++i)
    {
        if (!ctx.shapes[i].absorbed)
        {
            sorted_indices.push_back(i);
        }
    }

    std::sort(sorted_indices.begin(), sorted_indices.end(),
              [&](size_t a, size_t b)
              {
                  return ctx.shapes[a].bbox.min_x < ctx.shapes[b].bbox.min_x;
              });

    // Для каждой фигуры ищем соседей в окне по X через бинарный поиск, затем фильтруем по Y
    for (size_t i = 0; i < sorted_indices.size(); ++i)
    {
        size_t shape_idx = sorted_indices[i];
        const Shape &shape = ctx.shapes[shape_idx];
        const Pattern &pat_shape = ctx.pattern_registry[shape.pattern_idx];

        int64_t x_min = shape.bbox.min_x;
        int64_t x_max = shape.bbox.min_x + win_w;

        // Бинарный поиск: первый элемент с bbox.min_x >= x_min
        // (все элементы до i уже имеют меньший или равный x, но нам нужны и те что слева от shape в пределах окна)
        size_t lo = 0, hi = 0;
        {
            size_t left = 0, right = i;
            while (left < right)
            {
                size_t mid = (left + right) / 2;
                if (ctx.shapes[sorted_indices[mid]].bbox.min_x < x_min)
                {
                    left = mid + 1;
                }
                else
                {
                    right = mid;
                }
            }
            lo = left;
        }
        // Верхняя граница по X
        {
            size_t left = i + 1, right = sorted_indices.size();
            while (left < right)
            {
                size_t mid = (left + right) / 2;
                if (ctx.shapes[sorted_indices[mid]].bbox.min_x <= x_max)
                {
                    left = mid + 1;
                }
                else
                {
                    right = mid;
                }
            }
            hi = left;
        }

        for (size_t j = lo; j < hi; ++j)
        {
            size_t neighbor_idx = sorted_indices[j];
            if (neighbor_idx == shape_idx)
            {
                continue;
            }
            if (ctx.shapes[neighbor_idx].absorbed)
            {
                continue;
            }
            const Shape &neighbor = ctx.shapes[neighbor_idx];
            const Pattern &pat_neighbor = ctx.pattern_registry[neighbor.pattern_idx];

            // Фильтр по Y
            int64_t dy = std::abs(neighbor.bbox.min_y - shape.bbox.min_y);
            if (dy > win_h)
            {
                continue;
            }

            // Проверка что пара целиком в окне
            int64_t anchor_x = shape.bbox.min_x + pat_shape.bbox.min_x;
            int64_t anchor_y = shape.bbox.min_y + pat_shape.bbox.min_y;
            int64_t shape_max_x = shape.bbox.min_x + pat_shape.bbox.max_x;
            int64_t shape_max_y = shape.bbox.min_y + pat_shape.bbox.max_y;

            int64_t nbr_min_x = neighbor.bbox.min_x + pat_neighbor.bbox.min_x;
            int64_t nbr_min_y = neighbor.bbox.min_y + pat_neighbor.bbox.min_y;
            int64_t nbr_max_x = neighbor.bbox.min_x + pat_neighbor.bbox.max_x;
            int64_t nbr_max_y = neighbor.bbox.min_y + pat_neighbor.bbox.max_y;

            int64_t pair_min_x = std::min(anchor_x, nbr_min_x);
            int64_t pair_min_y = std::min(anchor_y, nbr_min_y);
            int64_t pair_max_x = std::max(shape_max_x, nbr_max_x);
            int64_t pair_max_y = std::max(shape_max_y, nbr_max_y);

            if (pair_max_x - pair_min_x > win_w)
            {
                continue;
            }
            if (pair_max_y - pair_min_y > win_h)
            {
                continue;
            }

            // Избегаем дублирования пары (i,j) и (j,i). берем только j > i по позиции в sorted_indices
            if (neighbor_idx < shape_idx && j < i)
            {
                continue;
            }

            NeighborKey key;
            key.offset_x = neighbor.bbox.min_x - shape.bbox.min_x;
            key.offset_y = neighbor.bbox.min_y - shape.bbox.min_y;
            key.pattern_idx_a = shape.pattern_idx;
            key.pattern_idx_b = neighbor.pattern_idx;

            pattern_pairs[key].push_back({shape_idx, neighbor_idx});
        }
    }

    return pattern_pairs;
}

static bool expand_patterns_once(GDSContext &gds_context, int64_t win_w, int64_t win_h)
{
    auto pattern_pairs = register_all_shape_pairs_as_patterns_fast(gds_context, win_w, win_h);

    auto max_it = std::max_element(
        pattern_pairs.begin(),
        pattern_pairs.end(),
        [](const auto &a, const auto &b)
        {
            return a.second.size() < b.second.size();
        });

    if (max_it == pattern_pairs.end())
    {
        return false;
    }

    if (max_it->second.size() == 0)
    {
        return false;
    }

    Pattern new_pattern;

    new_pattern.pattern_elements.push_back({{0, 0}, gds_context.shapes[max_it->second[0].shape_idx].pattern_idx});
    new_pattern.pattern_elements.push_back({{max_it->first.offset_x, max_it->first.offset_y}, gds_context.shapes[max_it->second[0].neighbor_idx].pattern_idx});
    std::sort(new_pattern.pattern_elements.begin(), new_pattern.pattern_elements.end());

    int64_t min_x = INT64_MAX, min_y = INT64_MAX;
    int64_t max_x = INT64_MIN, max_y = INT64_MIN;

    for (const Pattern::PatternElement &pe : new_pattern.pattern_elements)
    {
        const Pattern &child = gds_context.pattern_registry[pe.pattern_idx];
        min_x = std::min(min_x, pe.position_in_pattern.x + child.bbox.min_x);
        min_y = std::min(min_y, pe.position_in_pattern.y + child.bbox.min_y);
        max_x = std::max(max_x, pe.position_in_pattern.x + child.bbox.max_x);
        max_y = std::max(max_y, pe.position_in_pattern.y + child.bbox.max_y);
    }

    new_pattern.bbox.min_x = min_x;
    new_pattern.bbox.min_y = min_y;
    new_pattern.bbox.max_x = max_x;
    new_pattern.bbox.max_y = max_y;

    new_pattern.canonical_hash = compute_pattern_hash(new_pattern);

    size_t new_pattern_idx = gds_context.pattern_registry.get_or_insert(new_pattern.canonical_hash, new_pattern);

    for (const PotentialPattern &potential_pattern : pattern_pairs[max_it->first])
    {
        if (gds_context.shapes[potential_pattern.shape_idx].absorbed)
        {
            continue;
        }

        if (gds_context.shapes[potential_pattern.neighbor_idx].absorbed)
        {
            continue;
        }

        gds_context.shapes[potential_pattern.shape_idx].pattern_idx = new_pattern_idx;
        gds_context.shapes[potential_pattern.neighbor_idx].absorbed = true;
    }

    return true;
}

static void collect_primitives(const GDSContext &ctx, size_t pattern_idx, IntVec2 offset, vector<std::pair<IntVec2, size_t>> &result)
{
    const Pattern &pattern = ctx.pattern_registry[pattern_idx];

    if (pattern.is_leaf)
    {
        // pattern_elements[0].pattern_idx здесь — это primitive_idx
        const Pattern::PatternElement &pe = pattern.pattern_elements[0];
        result.push_back({{offset.x + pe.position_in_pattern.x, offset.y + pe.position_in_pattern.y}, pe.pattern_idx});
        return;
    }

    for (const Pattern::PatternElement &pe : pattern.pattern_elements)
    {
        collect_primitives(ctx, pe.pattern_idx, {offset.x + pe.position_in_pattern.x, offset.y + pe.position_in_pattern.y}, result);
    }
}

static void save_patterns_to_gds(const GDSContext &ctx, const string &output_dir)
{
    fs::create_directories(output_dir);

    string primitives_dir = output_dir + "/primitives";
    fs::create_directories(primitives_dir);

    const string txt_path = output_dir + "/patterns.txt";
    std::ofstream txt(txt_path);
    if (!txt.is_open())
    {
        cerr << "Failed to open: " << txt_path << endl;
        return;
    }

    unordered_map<size_t, vector<size_t>> pattern_to_shapes;
    for (size_t si = 0; si < ctx.shapes.size(); si++)
    {
        const Shape &s = ctx.shapes[si];
        if (s.absorbed)
        {
            continue;
        }

        pattern_to_shapes[s.pattern_idx].push_back(si);
    }

    int file_idx = 1;
    for (const auto &[pid, shape_indices] : pattern_to_shapes)
    {
        bool is_leaf = ctx.pattern_registry[pid].is_leaf;
        string base_dir = is_leaf ? primitives_dir : output_dir;
        string prefix = is_leaf ? "primitive_" : "pattern_";

        gdstk::Library lib = {};
        lib.init("pattern_lib", 1e-6, 1e-9);

        gdstk::Cell *cell = (gdstk::Cell *)calloc(1, sizeof(gdstk::Cell));
        cell->init("pattern");

        // Раскрываем паттерн — получаем примитивы с offset от (0,0) паттерна
        vector<std::pair<IntVec2, size_t>> primitives;
        collect_primitives(ctx, pid, {0, 0}, primitives);

        const IntVec2 anchor_pos = {
            ctx.shapes[shape_indices[0]].bbox.min_x,
            ctx.shapes[shape_indices[0]].bbox.min_y};

        for (const auto &[rel_pos, prim_idx] : primitives)
        {
            const Primitive &prim = ctx.primitive_registry[prim_idx];

            // bbox примитива начинается в (0,0) после канонизации, но position_on_canvas может отличаться от bbox.min. Нужно найти bbox.min примитива в его локальных координатах.
            int64_t prim_bbox_min_x = prim.canonical_points[0].x;
            int64_t prim_bbox_min_y = prim.canonical_points[0].y;
            for (const IntVec2 &cp : prim.canonical_points)
            {
                prim_bbox_min_x = std::min(prim_bbox_min_x, cp.x);
                prim_bbox_min_y = std::min(prim_bbox_min_y, cp.y);
            }

            gdstk::Polygon *poly = new gdstk::Polygon{};
            poly->tag = gdstk::make_tag((uint32_t)prim.layer_id, 0);

            for (const IntVec2 &cp : prim.canonical_points)
            {
                gdstk::Vec2 abs = {
                    from_grid(anchor_pos.x + rel_pos.x + cp.x - prim.bbox_min.x, ctx.grid_step),
                    from_grid(anchor_pos.y + rel_pos.y + cp.y - prim.bbox_min.y, ctx.grid_step)};
                poly->point_array.append(abs);
            }

            cell->polygon_array.append(poly);
        }

        lib.cell_array.append(cell);

        string fname = base_dir + "/" + prefix + std::to_string(file_idx) + ".gds";
        lib.write_gds(fname.c_str(), 0, NULL);
        lib.free_all();

        txt << fname << " <-> ";
        for (size_t i = 0; i < shape_indices.size(); ++i)
        {
            const Shape &s = ctx.shapes[shape_indices[i]];
            double lx = from_grid(s.bbox.min_x, ctx.grid_step);
            double ly = from_grid(s.bbox.min_y, ctx.grid_step);
            txt << "(" << lx << "," << ly << ")";
            if (i + 1 < shape_indices.size())
            {
                txt << ", ";
            }
        }
        txt << "\n";

        file_idx++;
    }

    cout << "[save] placements written to " << txt_path << "\n";
}

static GDSContext read_gds_file(const string &filepath)
{
    GDSContext gds_context;

    double file_unit = 0;
    double file_precision = 0;
    gdstk::gds_units(filepath.c_str(), file_unit, file_precision);

    gds_context.grid_step = file_precision / file_unit;

    cout << "[read_gds] unit=" << file_unit << "  precision=" << file_precision << "  grid_step=" << gds_context.grid_step << "\n";

    gdstk::ErrorCode err = gdstk::ErrorCode::NoError;
    gdstk::Library lib = gdstk::read_gds(filepath.c_str(), 0, 0, nullptr, &err);

    if (err != gdstk::ErrorCode::NoError)
    {
        cerr << "[read_gds] error reading file: " << filepath << "\n";
        lib.free_all();
        return gds_context;
    }

    gdstk::Array<gdstk::Cell *> top_cells = {};
    gdstk::Array<gdstk::RawCell *> top_rawcells = {};
    lib.top_level(top_cells, top_rawcells);

    for (uint64_t ci = 0; ci < top_cells.count; ++ci)
    {
        gdstk::Cell *cell = top_cells[ci];

        gdstk::Array<gdstk::Polygon *> flat_polys = {};
        cell->get_polygons(true, true, -1, false, 0, flat_polys);

        for (uint64_t pi = 0; pi < flat_polys.count; ++pi)
        {
            gdstk::Polygon *polygon = flat_polys[pi];

            uint32_t layer = gdstk::get_layer(polygon->tag);

            // Primitive конвертирует double → int64_t через grid_step внутри
            Primitive primitive(polygon->point_array, layer, gds_context.grid_step);
            size_t primitive_idx = gds_context.primitive_registry.get_or_insert(primitive.canonical_hash, primitive);

            // Начальный Pattern из одного примитива
            Pattern pattern;
            pattern.is_leaf = true;
            pattern.bbox.min_x = primitive.bbox_min.x;
            pattern.bbox.min_y = primitive.bbox_min.y;

            int64_t max_x = primitive.canonical_points[0].x;
            int64_t max_y = primitive.canonical_points[0].y;
            for (const IntVec2 &cp : primitive.canonical_points)
            {
                max_x = std::max(max_x, cp.x);
                max_y = std::max(max_y, cp.y);
            }
            pattern.bbox.max_x = max_x;
            pattern.bbox.max_y = max_y;

            Pattern::PatternElement pattern_element;
            pattern_element.position_in_pattern = {0, 0};
            pattern_element.pattern_idx = primitive_idx; // добавляем примитив, потому что только 1 pattern_element
            pattern.pattern_elements.push_back(pattern_element);
            pattern.canonical_hash = compute_pattern_hash(pattern);

            size_t pattern_idx = gds_context.pattern_registry.get_or_insert(pattern.canonical_hash, pattern);

            // position_on_canvas = минимальная точка полигона в int64_t
            int start = get_minimal_point_idx(polygon->point_array);
            gdstk::Vec2 raw = polygon->point_array[start];
            IntVec2 position = {
                to_grid(raw.x, gds_context.grid_step),
                to_grid(raw.y, gds_context.grid_step)};

            Shape shape;
            shape.position_on_canvas = position;
            shape.compute_bbox(gds_context.primitive_registry[primitive_idx]);
            shape.pattern_idx = pattern_idx;

            gds_context.shapes.push_back(shape);

            polygon->clear();
            gdstk::free_allocation(polygon);
        }

        flat_polys.clear();
    }

    top_cells.clear();
    top_rawcells.clear();
    lib.free_all();

    cout << "[read_gds] shapes: " << gds_context.shapes.size()
         << "  primitives: " << gds_context.primitive_registry.size()
         << "  initial patterns: " << gds_context.pattern_registry.size() << "\n";

    return gds_context;
}

int main()
{
    Config config;

    cout << "Input GDS file: ";
    std::getline(std::cin, config.input_file);

    cout << "Output directory [./output_patterns]: ";
    std::string out;
    std::getline(std::cin, out);
    if (!out.empty())
    {
        config.output_dir = out;
    }

    cout << "Window width in um [5.0]: ";
    std::string w;
    std::getline(std::cin, w);
    if (!w.empty())
    {
        config.win_w_um = std::stod(w);
    }

    cout << "Window height in um [5.0]: ";
    std::string h;
    std::getline(std::cin, h);
    if (!h.empty())
    {
        config.win_h_um = std::stod(h);
    }

    cout << "Max iterations [100]: ";
    std::string n;
    std::getline(std::cin, n);
    if (!n.empty())
    {
        config.max_iter = std::stoi(n);
    }

    cout << "\n[config] input:   " << config.input_file << "\n"
         << "[config] output:  " << config.output_dir << "\n"
         << "[config] win_w:   " << config.win_w_um << " um\n"
         << "[config] win_h:   " << config.win_h_um << " um\n"
         << "[config] max_iter:" << config.max_iter << "\n";

    GDSContext gds_context = read_gds_file(config.input_file);
    if (gds_context.shapes.empty())
    {
        cerr << "[main] no shapes loaded\n";
        return 1;
    }

    int64_t win_w = (int64_t)std::round(config.win_w_um / gds_context.grid_step);
    int64_t win_h = (int64_t)std::round(config.win_h_um / gds_context.grid_step);

    cout << "[main] window: " << win_w << " x " << win_h << " grid units\n";

    for (int i = 0; i < config.max_iter; i++)
    {
        cout << "iteration " << i + 1 << " / " << config.max_iter << "\n";

        if (!expand_patterns_once(gds_context, win_w, win_h))
        {
            cout << "[main] converged at iteration " << i + 1 << "\n";
            break;
        }
    }

    save_patterns_to_gds(gds_context, config.output_dir);
    return 0;
}