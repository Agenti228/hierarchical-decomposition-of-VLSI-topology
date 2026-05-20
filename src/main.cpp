#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <gdstk/gdstk.hpp>

using namespace std;

namespace fs = std::filesystem;

struct Shape
{
    int layer = 0;

    vector<gdstk::Vec2> points;

    double min_x = 0;
    double min_y = 0;

    double max_x = 0;
    double max_y = 0;
};

struct ShapeInstance
{
    int template_id = 0;

    double x;
    double y;
};

struct PatternElement
{
    int template_id = 0;

    double dx;
    double dy;

    bool operator<(const PatternElement &other) const
    {
        if (dx != other.dx)
        {
            return dx < other.dx;
        }

        if (dy != other.dy)
        {
            return dy < other.dy;
        }

        return template_id < other.template_id;
    }
};

struct Pattern
{
    int id = 0;

    vector<PatternElement> elements;

    vector<gdstk::Vec2> placements;
};

static void compute_bbox(Shape &shape)
{
    shape.min_x = shape.max_x = shape.points[0].x;
    shape.min_y = shape.max_y = shape.points[0].y;

    for (auto &point : shape.points)
    {
        shape.min_x = min(shape.min_x, point.x);
        shape.min_y = min(shape.min_y, point.y);

        shape.max_x = max(shape.max_x, point.x);
        shape.max_y = max(shape.max_y, point.y);
    }
}

static string build_canonical_shape(const Shape &shape)
{
    vector<gdstk::Vec2> points = shape.points;

    int count = static_cast<int>(points.size());

    int start_index = 0;

    for (int i = 1; i < count; ++i)
    {
        if (points[i] < points[start_index])
        {
            start_index = i;
        }
    }

    vector<gdstk::Vec2> cw_points;

    for (int i = 0; i < count; ++i)
    {
        cw_points.push_back(points[(static_cast<std::vector<gdstk::Vec2, std::allocator<gdstk::Vec2>>::size_type>(start_index) + i) % count]);
    }

    vector<gdstk::Vec2> ccw_points;

    for (int i = 0; i < count; ++i)
    {
        int index =
            (start_index - i + count) % count;

        ccw_points.push_back(points[index]);
    }

    auto build_string = [&](const vector<gdstk::Vec2> &polygon)
    {
        stringstream ss;

        ss << "L" << shape.layer << "|";

        gdstk::Vec2 anchor = polygon[0];

        ss << "0,0;";

        for (int i = 1; i < polygon.size(); ++i)
        {
            double dx = polygon[i].x - anchor.x;
            double dy = polygon[i].y - anchor.y;

            ss << dx << "," << dy << ";";
        }

        return ss.str();
    };

    string cw_string = build_string(cw_points);
    string ccw_string = build_string(ccw_points);

    return min(cw_string, ccw_string);
}

static string build_pattern_key(vector<PatternElement> elements)
{
    sort(elements.begin(), elements.end());

    stringstream ss;

    for (auto &element : elements)
    {
        ss
            << element.template_id
            << ":"
            << element.dx
            << ":"
            << element.dy
            << ";";
    }

    return ss.str();
}

static bool inside_window(const Shape &shape, double x, double y, double width, double height)
{
    return shape.min_x >= x &&
           shape.min_y >= y &&
           shape.max_x <= x + width &&
           shape.max_y <= y + height;
}

static vector<Shape> read_gds(const string &filename)
{
    struct LibraryGuard
    {
        gdstk::Library &library;

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
    LibraryGuard guard{library};

    if (error_code != gdstk::ErrorCode::NoError)
    {
        cerr << "Failed to read GDS file: " << filename << endl;
        cerr << "Error code: " << static_cast<int>(error_code) << endl;

        return {};
    }

    vector<Shape> shapes;

    for (size_t i = 0; i < library.cell_array.count; ++i)
    {
        const gdstk::Cell *cell = library.cell_array[i];

        if (cell == nullptr)
        {
            continue;
        }

        for (size_t j = 0; j < cell->polygon_array.count; ++j)
        {
            const gdstk::Polygon *polygon = cell->polygon_array[j];

            if (polygon == nullptr)
            {
                continue;
            }

            if (polygon->point_array.count == 0)
            {
                continue;
            }

            Shape shape;
            shape.points.reserve(polygon->point_array.count);

            shape.layer = gdstk::get_layer(polygon->tag);

            for (size_t k = 0; k < polygon->point_array.count; ++k)
            {
                const gdstk::Vec2 &point = polygon->point_array[k];

                shape.points.emplace_back(point);
            }

            compute_bbox(shape);

            shapes.push_back(shape);
        }
    }

    return shapes;
}

void write_pattern_gds(const Pattern &pattern, const vector<Shape> &template_shapes)
{
    string filename = "pattern" + to_string(pattern.id) + ".gds";

    gdstk::Library library = {};

    library.init("PATTERN_LIB", 1e-6, 1e-9);

    string cell_name = "PATTERN_" + to_string(pattern.id);

    gdstk::Cell *cell = (gdstk::Cell *)calloc(1, sizeof(gdstk::Cell));

    cell->init(cell_name.c_str());

    for (auto &element : pattern.elements)
    {
        const Shape &shape =
            template_shapes[element.template_id];

        gdstk::Polygon *polygon =
            new gdstk::Polygon();

        polygon->tag =
            gdstk::make_tag(shape.layer, 0);

        for (auto &point : shape.points)
        {
            polygon->point_array.append({point.x + element.dx, point.y + element.dy});
        }

        cell->polygon_array.append(polygon);
    }

    library.cell_array.append(cell);

    library.write_gds(filename.c_str(), 0, nullptr);

    library.free_all();
}

static void print_shapes(const vector<Shape> &shapes)
{
    cout << "Shapes count: " << shapes.size() << endl;

    for (size_t i = 0; i < shapes.size(); ++i)
    {
        const Shape &shape = shapes[i];

        cout << "Shape #" << i << endl;
        cout << "Layer: " << shape.layer << endl;

        cout << "Bounding box:" << endl;
        cout << "min_x = " << shape.min_x << endl;
        cout << "min_y = " << shape.min_y << endl;
        cout << "max_x = " << shape.max_x << endl;
        cout << "max_y = " << shape.max_y << endl;

        cout << "Points:" << endl;

        for (size_t j = 0; j < shape.points.size(); ++j)
        {
            const gdstk::Vec2 &point = shape.points[j];

            cout << point.x << " " << point.y << endl;
        }

        cout << endl;
    }
}

int main()
{
    const string resource_dir = RESOURCES_PATH;

    string filename = resource_dir + "66.gds";

    cout << filename << endl;

    vector<Shape> shapes = read_gds(filename);

    print_shapes(shapes);

    return 0;

    double width = 5.0;
    double height = 5.0;

    cout
        << "Shapes loaded: "
        << shapes.size()
        << endl;

    unordered_map<string, int> shape_map;

    vector<Shape> template_shapes;

    vector<ShapeInstance> instances;

    for (auto &shape : shapes)
    {
        string canonical =
            build_canonical_shape(shape);

        int template_id;

        if (shape_map.count(canonical))
        {
            template_id =
                shape_map[canonical];
        }
        else
        {
            template_id =
                static_cast<int>(
                    template_shapes.size());

            shape_map[canonical] =
                template_id;

            template_shapes.push_back(shape);
        }

        instances.push_back({template_id,
                             shape.min_x,
                             shape.min_y});
    }

    unordered_map<string, int> pattern_map;

    vector<Pattern> patterns;

    for (int i = 0; i < instances.size(); ++i)
    {
        auto &anchor =
            instances[i];

        vector<PatternElement> elements;

        double origin_x = anchor.x;
        double origin_y = anchor.y;

        for (int j = 0; j < instances.size(); ++j)
        {
            auto &other =
                instances[j];

            auto &shape =
                shapes[j];

            if (!inside_window(
                    shape,
                    origin_x,
                    origin_y,
                    width,
                    height))
            {
                continue;
            }

            elements.push_back({other.template_id,
                                other.x - origin_x,
                                other.y - origin_y});
        }

        if (elements.empty())
        {
            continue;
        }

        string key =
            build_pattern_key(elements);

        if (!pattern_map.count(key))
        {
            Pattern pattern;

            pattern.id =
                static_cast<int>(
                    patterns.size()) +
                1;

            pattern.elements =
                elements;

            patterns.push_back(pattern);

            pattern_map[key] =
                pattern.id - 1;
        }

        int pattern_index =
            pattern_map[key];

        patterns[pattern_index]
            .placements
            .push_back({origin_x,
                        origin_y});
    }

    vector<Pattern> filtered_patterns;

    for (auto &pattern : patterns)
    {
        if (pattern.placements.size() < 2)
        {
            continue;
        }

        filtered_patterns.push_back(pattern);
    }

    for (auto &pattern : filtered_patterns)
    {
        write_pattern_gds(
            pattern,
            template_shapes);
    }

    ofstream placements_file(
        "placements.txt");

    for (auto &pattern : filtered_patterns)
    {
        placements_file
            << "pattern"
            << pattern.id
            << ".gds <-> ";

        for (auto &position : pattern.placements)
        {
            placements_file
                << "("
                << position.x
                << ","
                << position.y
                << ") ";
        }

        placements_file
            << "\n";
    }

    placements_file.close();

    cout
        << "Patterns found: "
        << filtered_patterns.size()
        << endl;

    return 0;
}