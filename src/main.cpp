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

struct ProgressBar
{
    string label;
    size_t total;
    size_t current = 0;

    size_t bar_width = 40;
    size_t update_every = 10000;

    ProgressBar(const string &label_, size_t total_)
    {
        label = label_;
        total = total_;
        print();
    }

    void tick()
    {
        current++;

        if (current % update_every == 0 || current >= total)
        {
            print();
        }
    }

    void done()
    {
        current = total;
        print();
        std::cout << "\n";
    }

private:
    void print() const
    {
        double pct = total ? (double)current / total : 1.0;
        size_t filled = (size_t)(pct * bar_width);

        std::cout << "\033[2K\r" << label << " [";

        for (size_t i = 0; i < bar_width; ++i)
        {
            std::cout << (i < filled ? '#' : '.');
        }

        std::cout << "] " << current << "/" << total << " " << (int)(pct * 100) << "%" << std::flush;
    }
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

    Primitive() = default;

    Primitive(const gdstk::Array<gdstk::Vec2> non_canonical_points, const size_t layer, double grid_step)
    {
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
        size_t primitive_idx = 0;

        bool operator==(const PatternElement &other) const
        {
            return position_in_pattern == other.position_in_pattern && primitive_idx == other.primitive_idx;
        }

        bool operator<(const PatternElement &other) const
        {
            if (position_in_pattern != other.position_in_pattern)
            {
                return position_in_pattern < other.position_in_pattern;
            }

            return primitive_idx < other.primitive_idx;
        }
    };

    uint64_t canonical_hash = 0;
    vector<PatternElement> pattern_elements;
};

static uint64_t compute_pattern_hash(const Pattern &pattern)
{
    uint64_t hash = 0;

    for (const Pattern::PatternElement &element : pattern.pattern_elements)
    {
        hash_combine(hash, element.position_in_pattern.x);
        hash_combine(hash, element.position_in_pattern.y);
        hash_combine(hash, element.primitive_idx);
    }

    return hash;
}

template <typename T>
struct Registry
{
    struct Entry
    {
        T object;
        vector<size_t> shape_indices;
    };

    unordered_map<uint64_t, size_t> canonical_key_to_entry_idx;
    vector<Entry> unique_entries;

    size_t get_or_insert(const uint64_t &key, const T &obj, size_t shape_idx)
    {
        auto [it, inserted] = canonical_key_to_entry_idx.emplace(key, unique_entries.size());

        if (inserted)
        {
            unique_entries.push_back({obj, {}});
        }

        unique_entries[it->second].shape_indices.push_back(shape_idx);
        return it->second;
    }

    size_t get_or_insert(const uint64_t &key, const T &obj)
    {
        auto [it, inserted] = canonical_key_to_entry_idx.emplace(key, unique_entries.size());

        if (inserted)
        {
            unique_entries.push_back({obj, {}});
        }

        return it->second;
    }

    void move_shape(int old_idx, int new_idx, int shape_idx)
    {
        auto &old_list = unique_entries[old_idx].shape_indices;

        old_list.erase(std::remove(old_list.begin(), old_list.end(), shape_idx), old_list.end());

        unique_entries[new_idx].shape_indices.push_back(shape_idx);
    }

    const T &operator[](size_t index) const
    {
        return unique_entries[index].object;
    }

    const vector<size_t> &shapes_of(int index) const
    {
        return unique_entries[index].shape_indices;
    }

    size_t size() const
    {
        return unique_entries.size();
    }

    bool contains(const uint64_t &key) const
    {
        return canonical_key_to_entry_idx.count(key) > 0;
    }

    int index_of(uint64_t k) const
    {
        return canonical_key_to_entry_idx.at(k);
    }
};

struct Shape
{
    struct Bbox
    {
        int64_t min_x = 0, max_y = 0;
        int64_t min_y = 0, max_x = 0;
    };

    uint64_t canvas_hash = 0;
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

    void compute_hash(const Registry<Pattern> &pattern_registry, const Registry<Primitive> &primitive_registry)
    {
        const Pattern &pat = pattern_registry[pattern_idx];

        vector<size_t> layers;
        for (const Pattern::PatternElement &pattern_element : pat.pattern_elements)
        {
            layers.push_back(primitive_registry[pattern_element.primitive_idx].layer_id);
        }

        std::sort(layers.begin(), layers.end());
        layers.erase(std::unique(layers.begin(), layers.end()), layers.end());

        canvas_hash = 0;
        hash_combine(canvas_hash, bbox.min_x);
        hash_combine(canvas_hash, bbox.min_y);
        for (size_t layer : layers)
        {
            hash_combine(canvas_hash, layer);
        }
    }

    void set_pattern_idx(size_t new_pattern_idx, const Registry<Pattern> &pattern_registry, const Registry<Primitive> &primitive_registry)
    {
        pattern_idx = new_pattern_idx;
        compute_hash(pattern_registry, primitive_registry);
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

    bool operator==(const NeighborKey &other) const
    {
        return offset_x == other.offset_x &&
               offset_y == other.offset_y;
    }
};

struct NeighborKeyHash
{
    size_t operator()(const NeighborKey &key) const
    {
        uint64_t hash = 0;
        hash_combine(hash, key.offset_x);
        hash_combine(hash, key.offset_y);
        return hash;
    }
};

static unordered_map<NeighborKey, int, NeighborKeyHash> build_neighborhood(size_t anchor_idx, const vector<Shape> &shapes, int64_t win_w, int64_t win_h)
{
    const Shape &anchor = shapes[anchor_idx];
    int64_t wx = anchor.bbox.min_x;
    int64_t wy = anchor.bbox.min_y;

    unordered_map<NeighborKey, int, NeighborKeyHash> nbr;

    for (size_t j = 0; j < shapes.size(); j++)
    {
        const Shape &s = shapes[j];

        if (s.absorbed)
        {
            continue;
        }

        if (!s.inside_window(wx, wy, win_w, win_h))
        {
            continue;
        }

        NeighborKey key;
        key.offset_x = s.bbox.min_x - wx;
        key.offset_y = s.bbox.min_y - wy;

        nbr[key] = j;
    }

    return nbr;
}

static bool expand_patterns_once(GDSContext &ctx, int64_t win_w, int64_t win_h)
{
    bool changed = false;

    size_t pattern_count = ctx.pattern_registry.size();

    ProgressBar pb("expanding patterns", pattern_count);

    for (size_t pattern_idx = 0; pattern_idx < pattern_count; pattern_idx++)
    {
        const vector<size_t> &shape_indices = ctx.pattern_registry.shapes_of(pattern_idx);
        if (shape_indices.size() < 2)
        {
            continue;
        }

        auto first_nbr = build_neighborhood(shape_indices[0], ctx.shapes, win_w, win_h);

        unordered_map<NeighborKey, int, NeighborKeyHash> occurrence_count;
        for (const auto &k : first_nbr)
        {
            occurrence_count[k.first] = 1;
        }

        for (size_t fi = 1; fi < shape_indices.size(); fi++)
        {
            auto nbr = build_neighborhood(shape_indices[fi], ctx.shapes, win_w, win_h);

            for (auto &[key, _] : nbr)
            {
                auto it = occurrence_count.find(key);
                if (it != occurrence_count.end())
                {
                    it->second++;
                }
            }
        }

        size_t total = shape_indices.size();
        vector<NeighborKey> intersection;
        for (auto &[key, cnt] : occurrence_count)
        {
            if (cnt == total)
            {
                intersection.push_back(key);
            }
        }

        if (intersection.size() <= 1)
        {
            continue;
        }

        Pattern new_pat;
        const Shape &anchor = ctx.shapes[shape_indices[0]];

        for (const NeighborKey &key : intersection)
        {
            int neighbor_shape_idx = first_nbr.at(key);
            const Shape &nbr_shape = ctx.shapes[neighbor_shape_idx];
            const Pattern &nbr_pat = ctx.pattern_registry[nbr_shape.pattern_idx];

            for (const Pattern::PatternElement &pt : nbr_pat.pattern_elements)
            {
                Pattern::PatternElement new_pt;
                new_pt.position_in_pattern = {
                    nbr_shape.position_on_canvas.x + pt.position_in_pattern.x - anchor.bbox.min_x,
                    nbr_shape.position_on_canvas.y + pt.position_in_pattern.y - anchor.bbox.min_y};
                new_pt.primitive_idx = pt.primitive_idx;
                new_pat.pattern_elements.push_back(new_pt);
            }
        }

        std::sort(new_pat.pattern_elements.begin(), new_pat.pattern_elements.end());
        new_pat.canonical_hash = compute_pattern_hash(new_pat);

        size_t new_pid = ctx.pattern_registry.get_or_insert(new_pat.canonical_hash, new_pat);

        for (const NeighborKey &key : intersection)
        {
            if (key.offset_x == 0 && key.offset_y == 0)
            {
                continue;
            }
            int nbr_idx = first_nbr.at(key);
            ctx.shapes[nbr_idx].absorbed = true;
        }

        for (int si : shape_indices)
        {
            ctx.pattern_registry.move_shape(pattern_idx, new_pid, si);
            ctx.shapes[si].set_pattern_idx(new_pid, ctx.pattern_registry, ctx.primitive_registry);
        }

        changed = true;
        pb.tick();
    }

    pb.done();
    return changed;
}

static void save_patterns_to_gds(const GDSContext &ctx, const string &output_dir)
{
    fs::create_directories(output_dir);
    ProgressBar pb("saving patterns", ctx.pattern_registry.size());

    int file_idx = 1;
    for (int pid = 0; pid < (int)ctx.pattern_registry.size(); ++pid)
    {
        const vector<size_t> &shape_indices = ctx.pattern_registry.shapes_of(pid);
        if (shape_indices.empty())
            continue;

        const Pattern &pat = ctx.pattern_registry[pid];

        gdstk::Library lib = {};
        lib.init("pattern_lib", 1e-6, 1e-9);

        gdstk::Cell *cell = (gdstk::Cell *)calloc(1, sizeof(gdstk::Cell));
        cell->init("TOP");

        for (const Pattern::PatternElement &pe : pat.pattern_elements)
        {
            const Primitive &prim = ctx.primitive_registry[pe.primitive_idx];

            gdstk::Polygon *poly = new gdstk::Polygon{};
            poly->tag = gdstk::make_tag(prim.layer_id, 0);

            for (const IntVec2 &cp : prim.canonical_points)
            {
                // Конвертируем целые единицы обратно в double мкм
                gdstk::Vec2 abs = {
                    from_grid(cp.x + pe.position_in_pattern.x, ctx.grid_step),
                    from_grid(cp.y + pe.position_in_pattern.y, ctx.grid_step)};
                poly->point_array.append(abs);
            }

            cell->polygon_array.append(poly);
        }

        lib.cell_array.append(cell);

        string fname = output_dir + "/pattern" + std::to_string(file_idx) + ".gds";
        lib.write_gds(fname.c_str(), 0, NULL);
        lib.free_all();

        file_idx++;
        pb.tick();
    }

    pb.done();
}

static GDSContext read_gds_file(const string &filepath)
{
    GDSContext ctx;

    double file_unit = 0;
    double file_precision = 0;
    gdstk::gds_units(filepath.c_str(), file_unit, file_precision);

    ctx.grid_step = file_precision / file_unit;

    cout << "[read_gds] unit=" << file_unit << "  precision=" << file_precision << "  grid_step=" << ctx.grid_step << "\n";

    gdstk::ErrorCode err = gdstk::ErrorCode::NoError;
    gdstk::Library lib = gdstk::read_gds(filepath.c_str(), 0, 0, nullptr, &err);

    if (err != gdstk::ErrorCode::NoError)
    {
        cerr << "[read_gds] error reading file: " << filepath << "\n";
        lib.free_all();
        return ctx;
    }

    gdstk::Array<gdstk::Cell *> top_cells = {};
    gdstk::Array<gdstk::RawCell *> top_rawcells = {};
    lib.top_level(top_cells, top_rawcells);

    uint64_t total_polys = 0;
    for (uint64_t ci = 0; ci < top_cells.count; ++ci)
    {
        gdstk::Cell *cell = top_cells[ci];

        gdstk::Array<gdstk::Polygon *> flat_polys = {};

        cell->get_polygons(true, true, -1, false, 0, flat_polys);

        total_polys += flat_polys.count;

        for (uint64_t pi = 0; pi < flat_polys.count; ++pi)
        {
            flat_polys[pi]->clear();
            gdstk::free_allocation(flat_polys[pi]);
        }

        flat_polys.clear();
    }

    ProgressBar pb("reading file", (int)total_polys);

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
            Primitive primitive(polygon->point_array, layer, ctx.grid_step);
            size_t primitive_idx = ctx.primitive_registry.get_or_insert(primitive.canonical_hash, primitive);

            // Начальный Pattern из одного примитива
            Pattern pat;
            Pattern::PatternElement pe;
            pe.position_in_pattern = {0, 0};
            pe.primitive_idx = primitive_idx;
            pat.pattern_elements.push_back(pe);
            pat.canonical_hash = compute_pattern_hash(pat);

            // position_on_canvas = минимальная точка полигона в int64_t
            int start = get_minimal_point_idx(polygon->point_array);
            gdstk::Vec2 raw = polygon->point_array[start];
            IntVec2 position = {
                to_grid(raw.x, ctx.grid_step),
                to_grid(raw.y, ctx.grid_step)};

            Shape shape;
            shape.position_on_canvas = position;
            shape.compute_bbox(ctx.primitive_registry[primitive_idx]);

            size_t shape_idx = ctx.shapes.size();
            ctx.shapes.push_back(shape);

            size_t pattern_idx = ctx.pattern_registry.get_or_insert(pat.canonical_hash, pat, shape_idx);

            ctx.shapes[shape_idx].set_pattern_idx(pattern_idx, ctx.pattern_registry, ctx.primitive_registry);

            polygon->clear();
            gdstk::free_allocation(polygon);

            pb.tick();
        }

        flat_polys.clear();
    }

    pb.done();

    top_cells.clear();
    top_rawcells.clear();
    lib.free_all();

    cout << "[read_gds] shapes: " << ctx.shapes.size()
         << "  primitives: " << ctx.primitive_registry.size()
         << "  initial patterns: " << ctx.pattern_registry.size() << "\n";

    return ctx;
}

int main()
{
    const string filepath = string(RESOURCES_PATH) + "big_ine_layer.gds";

    constexpr double WIN_W_UM = 5.0;
    constexpr double WIN_H_UM = 5.0;
    constexpr int MAX_ITER = 20;

    GDSContext ctx = read_gds_file(filepath);
    if (ctx.shapes.empty())
    {
        cerr << "[main] no shapes loaded\n";
        return 1;
    }

    int64_t win_w = (int64_t)std::round(WIN_W_UM / ctx.grid_step);
    int64_t win_h = (int64_t)std::round(WIN_H_UM / ctx.grid_step);

    cout << "[main] window: " << win_w << " x " << win_h << " grid units\n";

    for (int i = 0; i < MAX_ITER; ++i)
    {
        if (!expand_patterns_once(ctx, win_w, win_h))
        {
            break;
        }
    }

    save_patterns_to_gds(ctx, "./output_patterns");
}
