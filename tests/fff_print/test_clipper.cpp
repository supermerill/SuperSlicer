#include <catch2/catch.hpp>

#include "test_data.hpp"
#include "clipper/clipper_z.hpp"

using namespace Slic3r;

// Test case for an issue with duplicity vertices (same XY coordinates but differ in Z coordinates) in Clipper 6.2.9,
// (related to https://sourceforge.net/p/polyclipping/bugs/160/) that was fixed in Clipper 6.4.2.
SCENARIO("Clipper Z", "[ClipperZ]")
{
    ClipperLib_Z::Path subject;

    subject.emplace_back(-2000, -1000, 10);
    subject.emplace_back(-2000,  1000, 10);
    subject.emplace_back( 2000,  1000, 10);
    subject.emplace_back( 2000, -1000, 10);

    ClipperLib_Z::Path clip;
    clip.emplace_back(-1000, -2000, -5);
    clip.emplace_back(-1000,  2000, -5);
    clip.emplace_back( 1000,  2000, -5);
    clip.emplace_back( 1000, -2000, -5);

    ClipperLib_Z::Clipper clipper;
    clipper.ZFillFunction([](const ClipperLib_Z::IntPoint &e1bot, const ClipperLib_Z::IntPoint &e1top, const ClipperLib_Z::IntPoint &e2bot,
                             const ClipperLib_Z::IntPoint &e2top, ClipperLib_Z::IntPoint &pt) {
        pt.z() = 1;
    });

    clipper.AddPath(subject, ClipperLib_Z::ptSubject, false);
    clipper.AddPath(clip, ClipperLib_Z::ptClip, true);

    ClipperLib_Z::PolyTree polytree;
    ClipperLib_Z::Paths    paths;
    clipper.Execute(ClipperLib_Z::ctIntersection, polytree, ClipperLib_Z::pftNonZero, ClipperLib_Z::pftNonZero);
    ClipperLib_Z::PolyTreeToPaths(polytree, paths);

    REQUIRE(paths.size() == 1);
    REQUIRE(paths.front().size() == 2);
    for (const ClipperLib_Z::IntPoint &pt : paths.front())
        REQUIRE(pt.z() == 1);
}