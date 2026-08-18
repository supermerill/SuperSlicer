#ifndef PRUSAPARTS_H
#define PRUSAPARTS_H

#include <vector>
#include <libslic3r/ExPolygon.hpp>

class TestData : public std::vector<Slic3r::Polygon>
{
public:
    TestData(std::initializer_list<Slic3r::Points> polygons)
    {
        this->reserve(polygons.size());
        for (const Slic3r::Points &points : polygons)
            this->emplace_back(points);
    }
};
using TestDataEx = std::vector<Slic3r::ExPolygons>;

extern const TestData PRUSA_PART_POLYGONS;
extern const TestData PRUSA_STEGOSAUR_POLYGONS;
extern const TestDataEx PRUSA_PART_POLYGONS_EX;

#endif // PRUSAPARTS_H
