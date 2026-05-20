#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <gdstk/gdstk.hpp>

using gdstk::Vec2;
using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::unordered_map;
using std::vector;

namespace fs = std::filesystem;

template <typename T>
static void hash_combine(uint64_t &seed, const T &value)
{
    std::hash<T> hasher;

    seed ^= hasher(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

// возвращает начальную точку у примитива, для того, чтобы избежать их дупликации в Registry. берет, исходя из лексикографического упроядочивания, реализованного в gdstk::Vec2
template <typename T>
static int get_minimal_point_idx(const T &non_canonical_points)
{
    int start_index = 0;
    int points_count = static_cast<int>(non_canonical_points.count);

    for (int i = 1; i < points_count; ++i)
    {
        if (non_canonical_points[i] < non_canonical_points[start_index])
        {
            start_index = i;
        }
    }

    return start_index;
}

// используем чтобы на первом этапе выбрать все уникальные полигоны (фигуры, примитивы). добавляем их в Registy, чтобы использовать для будущих рассчетов. это основная структура, в которой храниться вся информация для восстановления форм объектов
struct ShapeTemplate
{
    uint64_t canonical_hash = 0;

    int layer_id = 0;

    vector<Vec2> canonical_points;

    ShapeTemplate()
    {
    }

    ShapeTemplate(const gdstk::Array<gdstk::Vec2> non_canonical_points, const int layer)
    {
        layer_id = layer;

        int points_count = static_cast<int>(non_canonical_points.count);
        int start_index = get_minimal_point_idx(non_canonical_points);

        for (int i = 0; i < points_count; ++i)
        {
            int current_point_index = ((start_index) + i) % points_count;
            gdstk::Vec2 canonized_point = non_canonical_points[current_point_index] - non_canonical_points[start_index];
            canonical_points.push_back(canonized_point);
        }
    }
};

// использем чтобы хранить всевозможные инстансы (position_on_canvas в абсолютных координатах) паттернов, которые встречаются файле. на первом шаге, после прочтения файла, если использовать вместе с ShapeTemplate, то можно будет восставносить файл, без лишних обращений к Pattern. на последующих этапах мы будем объединять паттерны (что соотносится с pattern_id), а следовательно и Shape (GDSContext.shapes будет уменьшаться в размере)
struct Shape
{
    Vec2 position_on_canvas = {};

    int pattern_id = -1;

    bool consumed = false; // временное решение, нужное для более легкого объедиенния нескольких фигур
};

// это та структура, которая отвечает за хранение паттернов из файла и их объединение. она состит из:
// 1. pattern_templates - все примитивы, что попали в окрестность определенной точки (которую мы выбираем как вершину 1 полигона. эта точки измеряется в абсолютных координатах). pattern_templates, в свою очередь просто wrapper, который нужен для добаления информации примитивам (ShapeTemplate) о том, где они находятся внутри этого Pattern
// 2. canonical_hash, который нужен для того, чтобы можно было быстро определить что это за Pattern. он вычисляется через относительные координаты примитивов (PatternTemplate, который, фактически, является ShapeTemplate), которые лежат внутри Pattern и всех их слоев
// важное уточнение: сам Pattern не знает где он находится, лишь то, что он существует и какие примитивы хранит. хранение всех паттернов и их соответвие с Shape происходит в Registry
struct Pattern
{
    // нужен для того, чтобы знать где находятся ShapeTemplate (примитивы) на Pattern, потому что Pattern может состоять из многих примитивов. объединять сами примитивы нельзя, потому что тогда потеряются данные о расположении полигонов (фигур) в файле. можно было бы хранить данные
    struct PatternTemplate
    {
        Vec2 position_in_pattern = {};

        int shape_template_id = -1;

        bool operator<(const PatternTemplate &other) const
        {
            if (position_in_pattern != other.position_in_pattern)
            {
                return position_in_pattern < other.position_in_pattern;
            }

            return shape_template_id < other.shape_template_id;
        }
    };

    uint64_t canonical_hash = 0;

    vector<PatternTemplate> pattern_templates;
};

// позволяет хранить структуры, такие как ShapeTemplate и Pattern. доступ и сравнение происходит по хешу, который храниться в переменной canonical_hash, которая есть у обеих структур. реализованы методы:
// 1. get_or_insert, которая принимает:
//      key: uint64_t - это хеш примитива (паттерна)
//      object: T - добавляемый примитив (паттерн)
// если в canonical_key_to_template_id_map уже есть примитив (паттерн), который ма хотим добвить, то метод вернет индекс этого паттерна, который храниться в unique_templates. если же такого объекта нет, то он добавит его в canonical_key_to_template_id_map и в конец unique_templates, после чего вернет индекс (размер unique_templates)
// 2. operator[] - позволяет прочитать данные в unique_templates, без возможности их изменить
// 3. size - возвращает количество добаленных (следовательно уникальных) примитивов (паттернов)
// 4. contains, принимает на вход key: uint64_t, и проверяет по нему, есть ли примитив (паттерн) с таким хешем (у этих объектов он храниться в переменной canonical_hash) в canonical_key_to_template_id_map, то есть и в unique_templates
template <typename T>
struct Registry
{
    unordered_map<uint64_t, int> canonical_key_to_template_id_map;

    vector<T> unique_templates;

    int get_or_insert(const uint64_t &key, const T &object)
    {
        auto it = canonical_key_to_template_id_map.find(key);

        if (it != canonical_key_to_template_id_map.end())
        {
            return it->second;
        }

        int id = static_cast<int>(unique_templates.size());

        canonical_key_to_template_id_map[key] = id;

        unique_templates.push_back(object);

        return id;
    }

    const T &operator[](int index) const
    {
        return unique_templates[index];
    }

    int size() const
    {
        return static_cast<int>(unique_templates.size());
    }

    bool contains(const uint64_t &key) const
    {
        return canonical_key_to_template_id_map.count(key) > 0;
    }
};

// структура-wrapper для всех контейнеров. она нужна для более легкой передачи данных между фукнциями
struct GDSContext
{
    Registry<ShapeTemplate> shape_template_registry;
    Registry<Pattern> pattern_registry;
    vector<Shape> shapes;
};

static uint64_t calculate_pattern_hash(const Pattern &pattern)
{
    uint64_t hash = 0;

    for (const Pattern::PatternTemplate &element : pattern.pattern_templates)
    {
        hash_combine(hash, element.position_in_pattern.x);
        hash_combine(hash, element.position_in_pattern.y);
        hash_combine(hash, element.shape_template_id);
    }

    return hash;
}

static bool is_inside_window(const Vec2 &origin, const Vec2 &point, double width, double height)
{
    double dx = point.x - origin.x;
    double dy = point.y - origin.y;

    if (dx < 0.0 || dy < 0.0)
    {
        return false;
    }

    if (dx > width || dy > height)
    {
        return false;
    }

    return true;
}

static Pattern build_local_pattern(const GDSContext &gds, const Shape &anchor_shape, double width, double height)
{
    Pattern pattern;

    for (const Shape &candidate_shape : gds.shapes)
    {
        if (candidate_shape.consumed)
        {
            continue;
        }

        if (!is_inside_window(anchor_shape.position_on_canvas, candidate_shape.position_on_canvas, width, height))
        {
            continue;
        }

        Pattern::PatternTemplate element;

        element.position_in_pattern = candidate_shape.position_on_canvas - anchor_shape.position_on_canvas;

        element.shape_template_id = candidate_shape.pattern_id;

        pattern.pattern_templates.push_back(element);
    }

    sort(pattern.pattern_templates.begin(), pattern.pattern_templates.end());

    pattern.canonical_hash = calculate_pattern_hash(pattern);

    return pattern;
}

static unordered_map<uint64_t, vector<int>> find_pattern_candidates(const GDSContext &gds, double width, double height)
{
    unordered_map<uint64_t, vector<int>> pattern_candidates;

    for (int i = 0; i < static_cast<int>(gds.shapes.size()); ++i)
    {
        const Shape &shape = gds.shapes[i];

        if (shape.consumed)
        {
            continue;
        }

        Pattern pattern = build_local_pattern(gds, shape, width, height);

        pattern_candidates[pattern.canonical_hash].push_back(i);
    }

    return pattern_candidates;
}

static void register_patterns(GDSContext &gds, const unordered_map<uint64_t, vector<int>> &pattern_candidates, double width, double height)
{
    vector<Shape> new_shapes;

    for (const auto &[hash, shape_indices] : pattern_candidates)
    {
        if (shape_indices.size() < 2)
        {
            continue;
        }

        const Shape &first_shape = gds.shapes[shape_indices[0]];

        Pattern pattern = build_local_pattern(gds, first_shape, width, height);

        int pattern_id = gds.pattern_registry.get_or_insert(hash, pattern);

        for (int shape_index : shape_indices)
        {
            Shape &old_shape = gds.shapes[shape_index];

            old_shape.consumed = true;

            Shape new_shape;

            new_shape.position_on_canvas = old_shape.position_on_canvas;

            new_shape.pattern_id = pattern_id;

            new_shapes.push_back(new_shape);
        }
    }

    for (const Shape &shape : new_shapes)
    {
        gds.shapes.push_back(shape);
    }
}

static void remove_consumed_shapes(GDSContext &gds)
{
    vector<Shape> alive_shapes;

    for (const Shape &shape : gds.shapes)
    {
        if (!shape.consumed)
        {
            alive_shapes.push_back(shape);
        }
    }

    gds.shapes = std::move(alive_shapes);
}

static bool extract_patterns_iteration(GDSContext &gds, double width, double height)
{
    int shapes_before = static_cast<int>(gds.shapes.size());

    auto pattern_candidates = find_pattern_candidates(gds, width, height);

    register_patterns(gds, pattern_candidates, width, height);

    remove_consumed_shapes(gds);

    int shapes_after = static_cast<int>(gds.shapes.size());

    return shapes_after != shapes_before;
}

static void extract_patterns(GDSContext &gds, double width, double height)
{
    while (true)
    {
        bool changed = extract_patterns_iteration(gds, width, height);

        if (!changed)
        {
            break;
        }
    }
}

// пока так, когда появиться чтение из файла, тогда заменим
static void initialize_test_gds(GDSContext &gds)
{
    ShapeTemplate rectangle_template;

    rectangle_template.layer_id = 1;

    rectangle_template.canonical_points =
        {
            Vec2{0, 0},
            Vec2{10, 0},
            Vec2{10, 10},
            Vec2{0, 10}};

    rectangle_template.canonical_hash = 1111;

    int rectangle_pattern_id = gds.shape_template_registry.get_or_insert(
        rectangle_template.canonical_hash,
        rectangle_template);

    Shape shape_a;

    shape_a.position_on_canvas = Vec2{100, 100};
    shape_a.pattern_id = rectangle_pattern_id;

    gds.shapes.push_back(shape_a);

    Shape shape_b;

    shape_b.position_on_canvas = Vec2{120, 100};
    shape_b.pattern_id = rectangle_pattern_id;

    gds.shapes.push_back(shape_b);

    Shape shape_c;

    shape_c.position_on_canvas = Vec2{300, 300};
    shape_c.pattern_id = rectangle_pattern_id;

    gds.shapes.push_back(shape_c);

    Shape shape_d;

    shape_d.position_on_canvas = Vec2{320, 300};
    shape_d.pattern_id = rectangle_pattern_id;

    gds.shapes.push_back(shape_d);

    Shape noise_shape;

    noise_shape.position_on_canvas = Vec2{1000, 1000};
    noise_shape.pattern_id = rectangle_pattern_id;

    gds.shapes.push_back(noise_shape);
}

static void print_shapes(const vector<Shape> &shapes, Registry<ShapeTemplate> &unique_templates)
{
    cout << "Shapes count: " << shapes.size() << endl;

    for (size_t i = 0; i < shapes.size(); ++i)
    {
        const Shape &shape = shapes[i];
        ShapeTemplate polygonTemplate = unique_templates[shape.pattern_id];

        cout << "Shape #" << i << endl;
        cout << "Layer: " << polygonTemplate.layer_id << endl;

        cout << "Template points:" << endl;

        for (size_t j = 0; j < polygonTemplate.canonical_points.size(); ++j)
        {
            const Vec2 &point = polygonTemplate.canonical_points[j];

            cout << point.x << " " << point.y << endl;
        }

        cout << "Placement points:" << endl;

        for (size_t j = 0; j < polygonTemplate.canonical_points.size(); ++j)
        {
            const gdstk::Vec2 &point = polygonTemplate.canonical_points[j] + shape.position_on_canvas;

            cout << point.x << " " << point.y << endl;
        }

        cout << endl;
    }
}

int main()
{
    GDSContext gds;

    initialize_test_gds(gds);

    cout << "Before extraction:" << endl;

    print_shapes(gds.shapes, gds.shape_template_registry);

    double width = 40.0;
    double height = 40.0;

    extract_patterns(gds, width, height);

    cout << endl;
    cout << "After extraction:" << endl;

    cout << "Patterns found: " << gds.pattern_registry.size() << endl;

    return 0;
}
