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
static int get_start_index(const T& points, int count)
{
    int start_index = 0;

    for (int i = 1; i < count; ++i)
    {
        if (points[i] < points[start_index])
        {
            start_index = i;
        }
    }

    return start_index;
}

struct ShapeTemplate
{
    int layer_id = 0;

    vector<Vec2> canonical_points;

    ShapeTemplate()
    {
    }

    ShapeTemplate(const gdstk::Array<gdstk::Vec2>& non_canonical_points, const int layer)
    {
        int count = static_cast<int>(non_canonical_points.count);
        int start_index = get_start_index(non_canonical_points, count);
        layer_id = layer;

        for (int i = 0; i < count; ++i)
        {
            gdstk::Vec2 canonized_point = non_canonical_points[(start_index + i) % count] - non_canonical_points[start_index];
            canonical_points.push_back(canonized_point);
        }
    }
};

struct Shape
{
    Vec2 position_on_canvas = {};

    int shape_template_id = -1;
};

struct Pattern
{
    struct PatternTemaplate
    {
        Vec2 position_in_pattern = {};

        int shape_template_id = -1;

        bool operator<(const PatternTemaplate& other) const
        {
            if (position_in_pattern != other.position_in_pattern)
            {
                return position_in_pattern < other.position_in_pattern;
            }

            return shape_template_id < other.shape_template_id;
        }
    };

    string canonical_key;

    vector<PatternTemaplate> pattern_templates;

    vector<Vec2> pattern_template_positions;
};

template <typename T>
struct Registry
{
    unordered_map<string, int> canonical_key_to_template_id_map;

    vector<T> unique_templates;

    int get_or_insert(const string& key, const T& object)
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

    const T& operator[](int index) const
    {
        return unique_templates[index];
    }

    int size() const
    {
        return static_cast<int>(unique_templates.size());
    }

    bool contains(const string& key) const
    {
        return canonical_key_to_template_id_map.count(key) > 0;
    }
};

struct GDSContext
{
    Registry<ShapeTemplate> shape_template_registry;
    Registry<Pattern> pattern_registry;
    vector<Shape> shapes;
};

static string build_string(int layer, const vector<Vec2>& polygon)
{
    std::stringstream resultString;

    resultString << "L" << layer << "|";

    Vec2 anchor = polygon[0];

    resultString << "0,0;";

    for (size_t i = 1; i < polygon.size(); ++i)
    {
        double dx = polygon[i].x - anchor.x;
        double dy = polygon[i].y - anchor.y;

        resultString << dx << "," << dy << ";";
    }

    return resultString.str();
};


static void flatten(gdstk::Cell* cell, bool apply_repetitions) {
    uint64_t i = 0;
    while (i < cell->reference_array.count) {
        gdstk::Reference* ref = cell->reference_array[i];
        if (ref->type == gdstk::ReferenceType::Cell) {
            cell->reference_array.remove_unordered(i);
            ref->get_polygons(apply_repetitions, false, -1, false, 0, cell->polygon_array);
            ref->get_flexpaths(apply_repetitions, -1, false, 0, cell->flexpath_array);
            ref->get_robustpaths(apply_repetitions, -1, false, 0, cell->robustpath_array);
            ref->get_labels(apply_repetitions, -1, false, 0, cell->label_array);
        }
        else {
            ++i;
        }
    }
}

static vector<Shape> read_gds(const string& filename, Registry<ShapeTemplate>& unique_templates)
{
    struct LibraryGuard
    {
        gdstk::Library& library;

        ~LibraryGuard()
        {
            library.free_all();
        }
    };

    if (!fs::exists(filename))
    {
        cerr << "File does not exist: " << filename << endl;

        return {};
    }

    gdstk::ErrorCode error_code = gdstk::ErrorCode::NoError;

    gdstk::Library library = gdstk::read_gds(filename.c_str(), 1e-6, 1e-9, nullptr, &error_code);
    LibraryGuard guard{ library };

    if (error_code != gdstk::ErrorCode::NoError || error_code != gdstk::ErrorCode::MissingReference)
    {
        cerr << "Failed to read GDS file: " << filename << endl;
        cerr << "Error code: " << static_cast<int>(error_code) << endl;

        return {};
    }

    unordered_map<string, bool> referenced_cells;

    for (size_t i = 0; i < library.cell_array.count; ++i)
    {
        const gdstk::Cell* cell = library.cell_array[i];
        if (!cell) continue;

        for (size_t j = 0; j < cell->reference_array.count; ++j)
        {
            gdstk::Reference* ref = cell->reference_array[j];

            if (ref && ref->type == gdstk::ReferenceType::Cell && ref->cell)
            {
                referenced_cells[ref->cell->name] = true;
            }
        }
    }

    vector<Shape> shapes;

    for (size_t i = 0; i < library.cell_array.count; ++i)
    {
        gdstk::Cell* cell = library.cell_array[i];

        if (!cell) continue;

        if (referenced_cells.count(cell->name) > 0) continue;

        flatten(cell, true);

        for (size_t j = 0; j < cell->polygon_array.count; ++j)
        {
            const gdstk::Polygon* polygon = cell->polygon_array[j];

            if (!polygon || polygon->point_array.count == 0) continue;

            Shape shape;

            int start = get_start_index(polygon->point_array, static_cast<int>(polygon->point_array.count));
            shape.position_on_canvas = polygon->point_array[start];

            // ���������� gdstk::get_layer() ������ raw tag ��� ��� tag �������� � layer, � datatype
            int layer = static_cast<int>(gdstk::get_layer(polygon->tag));

            ShapeTemplate tmpl(polygon->point_array, layer);
            string key = build_string(tmpl.layer_id, tmpl.canonical_points);
            shape.shape_template_id = unique_templates.get_or_insert(key, tmpl);

            shapes.push_back(shape);
        }
    }

    return shapes;
}

static void print_shapes(const vector<Shape>& shapes, Registry<ShapeTemplate>& unique_templates)
{
    cout << "Shapes count: " << shapes.size() << endl;

    for (size_t i = 0; i < shapes.size(); ++i)
    {
        const Shape& shape = shapes[i];
        const ShapeTemplate& polygonTemplate = unique_templates[shape.shape_template_id];

        cout << "Shape #" << i << endl;
        cout << "Layer: " << polygonTemplate.layer_id << endl;

        cout << "Placement points (" << polygonTemplate.canonical_points.size() << "):" << endl;

        for (size_t j = 0; j < polygonTemplate.canonical_points.size(); ++j)
        {
            const gdstk::Vec2 point = polygonTemplate.canonical_points[j] + shape.position_on_canvas;

            cout << point.x << " " << point.y << endl;
        }

        cout << endl;
    }
}

static void save_templates_to_gds(
    const Registry<ShapeTemplate>& registry,
    const string& output_dir)
{
    fs::create_directories(output_dir);

    for (int i = 0; i < registry.size(); ++i)
    {
        const ShapeTemplate& tmpl = registry[i];

        gdstk::Library lib = {};
        lib.init("template_lib", 1e-6, 1e-9);

        gdstk::Cell* cell = (gdstk::Cell*)gdstk::allocate_clear(sizeof(gdstk::Cell));
        cell->init("template");

        gdstk::Polygon* poly = (gdstk::Polygon*)gdstk::allocate_clear(sizeof(gdstk::Polygon));

        poly->tag = gdstk::make_tag(static_cast<uint16_t>(tmpl.layer_id), 0);

        for (const Vec2& pt : tmpl.canonical_points)
        {
            poly->point_array.append(pt);
        }

        cell->polygon_array.append(poly);
        lib.cell_array.append(cell);

        string out_path = output_dir + "/template_"
            + std::to_string(i)
            + "_L" + std::to_string(tmpl.layer_id)
            + ".gds";

        gdstk::ErrorCode err = lib.write_gds(out_path.c_str(), 0, nullptr);

        if (err != gdstk::ErrorCode::NoError)
        {
            cerr << "Failed to write: " << out_path
                << " (error " << static_cast<int>(err) << ")" << endl;
        }
        else
        {
            cout << "Saved: " << out_path << endl;
        }

        lib.free_all();
    }
}

int main()
{
    GDSContext gds;

    const string resource_dir = RESOURCES_PATH;

    string filename = resource_dir + "big_all_layers.gds";

    cout << filename << endl;

    gds.shapes = read_gds(filename, gds.shape_template_registry);

    //print_shapes(gds.shapes, gds.shape_template_registry);

    save_templates_to_gds(gds.shape_template_registry, resource_dir + "templates");

    return 0;
}