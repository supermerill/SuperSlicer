
#include <catch_main.hpp>

#include <numeric>
#include <iostream>
#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/SVG.hpp"

using namespace Slic3r;

SCENARIO("Various Clipper operations - xs/t/11_clipper.t", "[ClipperUtils]") {
	// CCW oriented contour
	Slic3r::Polygon   square{ { 200, 100 }, {200, 200}, {100, 200}, {100, 100} };
	// CW oriented contour
	Slic3r::Polygon   hole_in_square{ { 160, 140 }, { 140, 140 }, { 140, 160 }, { 160, 160 } };
	Slic3r::ExPolygon square_with_hole(square, hole_in_square);
	GIVEN("square_with_hole") {
        WHEN("offset") {
            Polygons result = Slic3r::offset(square_with_hole, 5.f);
            THEN("offset matches") {
                REQUIRE(result == Polygons { 
                    { { 205, 205 }, { 95, 205 }, { 95, 95 }, { 205, 95 }, },
                    { { 145, 145 }, { 145, 155 }, { 155, 155 }, { 155, 145 } } });
            }
        }
        WHEN("offset_ex") {
            ExPolygons result = Slic3r::offset_ex(square_with_hole, 5.f);
            THEN("offset matches") {
                REQUIRE(result == ExPolygons { { 
                    { { 205, 205 }, { 95, 205 }, { 95, 95 }, { 205, 95 }, },
                    { { 145, 145 }, { 145, 155 }, { 155, 155 }, { 155, 145 } } } } );
            }
        }
        WHEN("offset2_ex") {
            ExPolygons result = Slic3r::offset2_ex({square_with_hole}, 5.f, -2.f);
            THEN("offset matches") {
                REQUIRE(result == ExPolygons { {
                    { { 203, 203 }, { 97, 203 }, { 97, 97 }, { 203, 97 } },
                    { { 143, 143 }, { 143, 157 }, { 157, 157 }, { 157, 143 } } } } );
            }
        }
    }
    GIVEN("square_with_hole 2") {
        Slic3r::ExPolygon square_with_hole(
            { { 20000000, 20000000 }, { 0, 20000000 }, { 0, 0 }, { 20000000, 0 } },
            { { 5000000, 15000000 }, { 15000000, 15000000 }, { 15000000, 5000000 }, { 5000000, 5000000 } });
        WHEN("offset2_ex") {
            Slic3r::ExPolygons result = Slic3r::offset2_ex(ExPolygons { square_with_hole }, -1.f, 1.f);
            THEN("offset matches") {
                REQUIRE(result.size() == 1);
                REQUIRE(square_with_hole.area() == result.front().area());
            }
        }
    }
    GIVEN("square and hole") {
        WHEN("diff_ex") {
            ExPolygons result = Slic3r::diff_ex(Polygons{ square }, Polygons{ hole_in_square });
            THEN("hole is created") {
                REQUIRE(result.size() == 1);
                REQUIRE(square_with_hole.area() == result.front().area());
            }
        }
    }
    GIVEN("polyline") {
        Slic3r::Polyline polyline { { 50, 150 }, { 300, 150 } };
        WHEN("intersection_pl") {
            Polylines result = Slic3r::intersection_pl({ polyline }, { square, hole_in_square });
            THEN("correct number of result lines") {
                REQUIRE(result.size() == 2);
            }
            THEN("result lines have correct length") {
                // results are in no particular order
                REQUIRE(result[0].length() == 40);
                REQUIRE(result[1].length() == 40);
            }
        }
        WHEN("diff_pl") {
            Polylines result = Slic3r::diff_pl(Polylines{ polyline }, Polygons{ square, hole_in_square });
            THEN("correct number of result lines") {
                REQUIRE(result.size() == 3);
            }
            // results are in no particular order
            THEN("the left result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Slic3r::Polyline &pl) { return pl.length() == 50; }) == 1);
            }
            THEN("the right result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Slic3r::Polyline &pl) { return pl.length() == 100; }) == 1);
            }
            THEN("the central result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Slic3r::Polyline &pl) { return pl.length() == 20; }) == 1);
            }
        }
    }
	GIVEN("Clipper bug #96 / Slic3r issue #2028") {
		Slic3r::Polyline subject{
			{ 44735000, 31936670 }, { 55270000, 31936670 }, { 55270000, 25270000 }, { 74730000, 25270000 }, { 74730000, 44730000 }, { 68063296, 44730000 }, { 68063296, 55270000 }, { 74730000, 55270000 },
			{ 74730000, 74730000 }, { 55270000, 74730000 }, { 55270000, 68063296 }, { 44730000, 68063296 }, { 44730000, 74730000 }, { 25270000, 74730000 }, { 25270000, 55270000 }, { 31936670, 55270000 },
			{ 31936670, 44730000 }, { 25270000, 44730000 }, { 25270000, 25270000 }, { 44730000, 25270000 }, { 44730000, 31936670 } };
		Slic3r::Polygon clip { {75200000, 45200000}, {54800000, 45200000}, {54800000, 24800000}, {75200000, 24800000} };
		Slic3r::Polylines result = Slic3r::intersection_pl(Polylines{ subject }, clip);
		THEN("intersection_pl - result is not empty") {
			REQUIRE(result.size() == 1); }
        result = Slic3r::intersection_pl(subject, Polygons{clip});
		THEN("intersection_pl(2) - result is not empty") {
			REQUIRE(result.size() == 1);
		}
	}
	GIVEN("Clipper bug #122") {
		Slic3r::Polyline subject { { 1975, 1975 }, { 25, 1975 }, { 25, 25 }, { 1975, 25 }, { 1975, 1975 } };
		Slic3r::Polygons clip { { { 2025, 2025 }, { -25, 2025 } , { -25, -25 }, { 2025, -25 } },
								{ { 525, 525 }, { 525, 1475 }, { 1475, 1475 }, { 1475, 525 } } };
		Slic3r::Polylines result = Slic3r::intersection_pl(Polylines{ subject }, clip);
		THEN("intersection_pl - result is not empty") {
			REQUIRE(result.size() == 1);
			REQUIRE(result.front().points.size() == 5);
		}
        result = Slic3r::intersection_pl(subject, Polygons{clip});
		THEN("intersection_pl(2) - result is not empty") {
			REQUIRE(result.size() == 1);
			REQUIRE(result.front().points.size() == 5);
		}
	}
	GIVEN("Clipper bug #126") {
		Slic3r::Polyline subject { { 200000, 19799999 }, { 200000, 200000 }, { 24304692, 200000 }, { 15102879, 17506106 }, { 13883200, 19799999 }, { 200000, 19799999 } };
		Slic3r::Polygon clip { { 15257205, 18493894 }, { 14350057, 20200000 }, { -200000, 20200000 }, { -200000, -200000 }, { 25196917, -200000 } };
		Slic3r::Polylines result = Slic3r::intersection_pl(Polylines{ subject }, clip);
		THEN("intersection_pl - result is not empty") {
			REQUIRE(result.size() == 1);
		}
		THEN("intersection_pl - result has same length as subject polyline") {
			REQUIRE(result.front().length() == Approx(subject.length()));
		}
        result = Slic3r::intersection_pl(subject, Polygons{clip});
		THEN("intersection_pl(2) - result is not empty") {
			REQUIRE(result.size() == 1);
		}
		THEN("intersection_pl(2) - result has same length as subject polyline") {
			REQUIRE(result.front().length() == Approx(subject.length()));
		}
        result = Slic3r::intersection_pl(Polylines{subject}, Polygons{clip});
		THEN("intersection_pl(3) - result is not empty") {
			REQUIRE(result.size() == 1);
		}
		THEN("intersection_pl(3) - result has same length as subject polyline") {
			REQUIRE(result.front().length() == Approx(subject.length()));
		}
	}

#if 0
	{
		# Clipper does not preserve polyline orientation
		my $polyline = Slic3r::Polyline->new([50, 150], [300, 150]);
		my $result = Slic3r::Geometry::Clipper::intersection_pl([$polyline], [$square]);
		is scalar(@$result), 1, 'intersection_pl - correct number of result lines';
		is_deeply $result->[0]->pp, [[100, 150], [200, 150]], 'clipped line orientation is preserved';
	}
	{
		# Clipper does not preserve polyline orientation
		my $polyline = Slic3r::Polyline->new([300, 150], [50, 150]);
		my $result = Slic3r::Geometry::Clipper::intersection_pl([$polyline], [$square]);
		is scalar(@$result), 1, 'intersection_pl - correct number of result lines';
		is_deeply $result->[0]->pp, [[200, 150], [100, 150]], 'clipped line orientation is preserved';
	}
	{
		# Disabled until Clipper bug #127 is fixed
		my $subject = [
			Slic3r::Polyline->new([-90000000, -100000000], [-90000000, 100000000]), # vertical
				Slic3r::Polyline->new([-100000000, -10000000], [100000000, -10000000]), # horizontal
				Slic3r::Polyline->new([-100000000, 0], [100000000, 0]), # horizontal
				Slic3r::Polyline->new([-100000000, 10000000], [100000000, 10000000]), # horizontal
		];
		my $clip = Slic3r::Polygon->new(# a circular, convex, polygon
			[99452190, 10452846], [97814760, 20791169], [95105652, 30901699], [91354546, 40673664], [86602540, 50000000],
			[80901699, 58778525], [74314483, 66913061], [66913061, 74314483], [58778525, 80901699], [50000000, 86602540],
			[40673664, 91354546], [30901699, 95105652], [20791169, 97814760], [10452846, 99452190], [0, 100000000],
			[-10452846, 99452190], [-20791169, 97814760], [-30901699, 95105652], [-40673664, 91354546],
			[-50000000, 86602540], [-58778525, 80901699], [-66913061, 74314483], [-74314483, 66913061],
			[-80901699, 58778525], [-86602540, 50000000], [-91354546, 40673664], [-95105652, 30901699],
			[-97814760, 20791169], [-99452190, 10452846], [-100000000, 0], [-99452190, -10452846],
			[-97814760, -20791169], [-95105652, -30901699], [-91354546, -40673664], [-86602540, -50000000],
			[-80901699, -58778525], [-74314483, -66913061], [-66913061, -74314483], [-58778525, -80901699],
			[-50000000, -86602540], [-40673664, -91354546], [-30901699, -95105652], [-20791169, -97814760],
			[-10452846, -99452190], [0, -100000000], [10452846, -99452190], [20791169, -97814760],
			[30901699, -95105652], [40673664, -91354546], [50000000, -86602540], [58778525, -80901699],
			[66913061, -74314483], [74314483, -66913061], [80901699, -58778525], [86602540, -50000000],
			[91354546, -40673664], [95105652, -30901699], [97814760, -20791169], [99452190, -10452846], [100000000, 0]
			);
		my $result = Slic3r::Geometry::Clipper::intersection_pl($subject, [$clip]);
		is scalar(@$result), scalar(@$subject), 'intersection_pl - expected number of polylines';
		is sum(map scalar(@$_), @$result), scalar(@$subject) * 2, 'intersection_pl - expected number of points in polylines';
	}
#endif
}

SCENARIO("Various Clipper operations - t/clipper.t", "[ClipperUtils]") {
    GIVEN("square with hole") {
        // CCW oriented contour
        Slic3r::Polygon   square { { 10, 10 }, { 20, 10 }, { 20, 20 }, { 10, 20 } };
        Slic3r::Polygon   square2 { { 5, 12 }, { 25, 12 }, { 25, 18 }, { 5, 18 } };
        // CW oriented contour
        Slic3r::Polygon   hole_in_square { { 14, 14 }, { 14, 16 }, { 16, 16 }, { 16, 14 } };
        WHEN("intersection_ex with another square") {
            ExPolygons intersection = Slic3r::intersection_ex(Polygons{ square, hole_in_square }, Polygons{ square2 });
            THEN("intersection area matches (hole is preserved)") {
                ExPolygon match({ { 20, 18 }, { 10, 18 }, { 10, 12 }, { 20, 12 } },
                                { { 14, 16 }, { 16, 16 }, { 16, 14 }, { 14, 14 } });
                REQUIRE(intersection.size() == 1);
                REQUIRE(intersection.front().area() == Approx(match.area()));
            }
        }
    }
    GIVEN("square with hole 2") {
        // CCW oriented contour
        Slic3r::Polygon   square { { 0, 0 }, { 40, 0 }, { 40, 40 }, { 0, 40 } };
        Slic3r::Polygon   square2 { { 10, 10 }, { 30, 10 }, { 30, 30 }, { 10, 30 } };
        // CW oriented contour
        Slic3r::Polygon   hole { { 15, 15 }, { 15, 25 }, { 25, 25 }, {25, 15 } };
        WHEN("union_ex with another square") {
            ExPolygons union_ = Slic3r::union_ex(Polygons{ square, square2, hole });
            THEN("union of two ccw and one cw is a contour with no holes") {
                REQUIRE(union_.size() == 1);
                REQUIRE(union_.front() == ExPolygon { { 40, 40 }, { 0, 40 }, { 0, 0 }, { 40, 0 } } );
            }
        }
        WHEN("diff_ex with another square") {
			ExPolygons diff = Slic3r::diff_ex(Polygons{ square, square2 }, Polygons{ hole });
            THEN("difference of a cw from two ccw is a contour with one hole") {
                REQUIRE(diff.size() == 1);
                REQUIRE(diff.front().area() == Approx(ExPolygon({ {40, 40}, {0, 40}, {0, 0}, {40, 0} }, { {15, 25}, {25, 25}, {25, 15}, {15, 15} }).area()));
            }
        }
    }
    GIVEN("yet another square") {
        Slic3r::Polygon  square { { 10, 10 }, { 20, 10 }, { 20, 20 }, { 10, 20 } };
        Slic3r::Polyline square_pl = square.split_at_first_point();
        WHEN("no-op diff_pl") {
            Slic3r::Polylines res = Slic3r::diff_pl({ square_pl }, {});
            THEN("returns the right number of polylines") {
                REQUIRE(res.size() == 1);
            }
            THEN("returns the unmodified input polyline") {
                REQUIRE(res.front().points.size() == square_pl.points.size());
            }
        }
    }
}

template<e_ordering o = e_ordering::OFF, class P, class Tree> 
double polytree_area(const Tree &tree, std::vector<P> *out)
{
    traverse_pt<o>(tree, out);
    
    return std::accumulate(out->begin(), out->end(), 0.0,
                           [](double a, const P &p) { return a + p.area(); });
}

size_t count_polys(const ExPolygons& expolys)
{
    size_t c = 0;
    for (auto &ep : expolys) c += ep.holes.size() + 1;
    
    return c;
}

TEST_CASE("Traversing Clipper PolyTree", "[ClipperUtils]") {
    // Create a polygon representing unit box
    Slic3r::Polygon unitbox;
    const int32_t UNIT = int32_t(1. / SCALING_FACTOR);
    unitbox.points = Points{Point{0, 0}, Point{UNIT, 0}, Point{UNIT, UNIT}, Point{0, UNIT}};
    
    Slic3r::Polygon box_frame = unitbox;
    box_frame.scale(20, 10);
    
    Slic3r::Polygon hole_left = unitbox;
    hole_left.scale(8);
    hole_left.translate(UNIT, UNIT);
    hole_left.reverse();
    
    Slic3r::Polygon hole_right = hole_left;
    hole_right.translate(UNIT * 10, 0);
    
    Slic3r::Polygon inner_left = unitbox;
    inner_left.scale(4);
    inner_left.translate(UNIT * 3, UNIT * 3);
    
    Slic3r::Polygon inner_right = inner_left;
    inner_right.translate(UNIT * 10, 0);
    
    Polygons reference = union_({box_frame, hole_left, hole_right, inner_left, inner_right});
    
    ClipperLib::PolyTree tree = union_pt(reference);
    double area_sum = box_frame.area() + hole_left.area() +
                      hole_right.area() + inner_left.area() +
                      inner_right.area();
    
    REQUIRE(area_sum > 0);

    SECTION("Traverse into Polygons WITHOUT spatial ordering") {
        Polygons output;
        REQUIRE(area_sum == Approx(polytree_area(tree.GetFirst(), &output)));
        REQUIRE(output.size() == reference.size());
    }
    
    SECTION("Traverse into ExPolygons WITHOUT spatial ordering") {
        ExPolygons output;
        REQUIRE(area_sum == Approx(polytree_area(tree.GetFirst(), &output)));
        REQUIRE(count_polys(output) == reference.size());
    }
    
    SECTION("Traverse into Polygons WITH spatial ordering") {
        Polygons output;
        REQUIRE(area_sum == Approx(polytree_area<e_ordering::ON>(tree.GetFirst(), &output)));
        REQUIRE(output.size() == reference.size());
    }
    
    SECTION("Traverse into ExPolygons WITH spatial ordering") {
        ExPolygons output;
        REQUIRE(area_sum == Approx(polytree_area<e_ordering::ON>(tree.GetFirst(), &output)));
        REQUIRE(count_polys(output) == reference.size());
    }
}

TEST_CASE("Testing offset and offset_ex ", "[ClipperUtils]") {
    
    SECTION("contour growth create hole")
    {
        std::vector<std::pair<double, double>> unscaled_array = {{0, 0}, {10, 0}, {10, 4.5}, {7, 4.5}, {7,3}, {3,3}, {3,7}, {7,7}, {7,5.5}, {10,5.5}, {10,10}, {0,10}};
        Slic3r::Polygon src_polygon;
        for(auto pt: unscaled_array)
            src_polygon.points.emplace_back(scale_t(pt.first), scale_t(pt.second));
        REQUIRE(src_polygon.is_valid());

        Polygons result = Slic3r::offset(src_polygon, scale_d(1), ClipperLib::JoinType::jtMiter, 3);
        REQUIRE(result.size() == 2);
        REQUIRE(result[0].is_counter_clockwise() || result[1].is_counter_clockwise());
        REQUIRE(result[0].is_clockwise() || result[1].is_clockwise());
        
        ExPolygons result_ex = Slic3r::offset_ex(ExPolygon(src_polygon), scale_d(1), ClipperLib::JoinType::jtMiter, 3);
        REQUIRE(result_ex.size() == 1);
        REQUIRE(result_ex[0].holes.size() == 1);
        REQUIRE(result_ex[0].contour.is_counter_clockwise());
        REQUIRE(result_ex[0].holes[0].is_clockwise());

        result_ex = union_ex(result);
        REQUIRE(result_ex.size() == 1);
        REQUIRE(result_ex[0].holes.size() == 1);
        REQUIRE(result_ex[0].contour.is_counter_clockwise());
        REQUIRE(result_ex[0].holes[0].is_clockwise());
    }
}

TEST_CASE("Testing ", "[ClipperUtils]") {
    Slic3r::Polygon src_polygon(
        {{-29766902, -30710288}, {-30290102, -30802646}, {-30799114, -30715083}, {-31876243, -30562718},
         {-33030941, -30449754}, {-33231822, -30436946}, {-34268178, -30384775}, {-34891023, -30367930},
         {-34938429, -30367343}, {-36307009, -30380364}, {-36686920, -30395327}, {-38057500, -30465424},
         {-38066183, -30465841}, {-39121269, -30543247}, {-39144586, -30545052}, {-41393647, -30762768},
         {-41400772, -30763367}, {-42606470, -30898534}, {-43049745, -30951762}, {-43526989, -31152101},
         {-44543970, -31296610}, {-49896253, -32067648}, {-53031149, -32453333}, {-54983432, -32629283},
         {-55876108, -32681239}, {-57207787, -32710144}, {-57287031, -32707371}, {-56999037, -31773098},
         {-57020109, -31574537}, {-57153102, -31460716}, {-59114378, -30732754}, {-59554951, -30452156},
         {-59664101, -30265002}, {-59753496, -29913462}, {-59728476, -29015470}, {-59648533, -27590938},
         {-59696516, -27427759}, {-59871882, -27324947}, {-60861946, -27207800}, {-60553293, -26514064},
         {-60221446, -25827699}, {-59983819, -25377161}, {-59431848, -24334493}, {-58071530, -22002475},
         {-57086298, -20406564}, {-54532068, -16383584}, {-54152045, -16033352}, {-53418323, -14810628},
         {-53037302, -14152026}, {-52585902, -13384179}, {-52093130, -12530959}, {-52089199, -12523696},
         {-51416049, -11301170}, {-51399188, -11269626}, {-50899221, -10293557}, {-50548785, -9599755},
         {-50422361, -9325954},  {-49913114, -8198227},  {-49857361, -8070473},  {-49486084, -7130146},
         {-49262185, -6546354},  {-48814997, -5175926},  {-48666648, -4650820},  {-48416355, -3640670},
         {-48173788, -2389333},  {-48059689, -1542776},  {-47989236, -963142},   {-47988421, -954092},
         {-47908090, 106824},    {-47878053, 573422},    {-47849952, 1687025},   {-47645107, 4755332},
         {-47768143, 5288883},   {-47768047, 5291706},   {-47527604, 7621018},   {-47663943, 7838131},
         {-47525823, 8455742},   {-47689343, 9155509},   {-47795210, 10268834},  {-47978714, 11428999},
         {-48194112, 12344043},  {-48481144, 13309478},  {-48642179, 13794190},  {-48842780, 14334161},
         {-49197836, 15187901},  {-49588991, 16033320},  {-49853153, 16562549},  {-50513053, 17792804},
         {-50882667, 18419696},  {-51438514, 19339116},  {-51718684, 19773192},  {-52179489, 20475205},
         {-52491489, 20942905},  {-52496021, 20949622},  {-53290936, 22086329},  {-53752870, 22724706},
         {-54177967, 23303509},  {-54181286, 23308060},  {-55141698, 24578760},  {-55144467, 24582493},
         {-55936527, 25607586},  {-56390354, 26180675},  {-56401601, 26194795},  {-57375148, 27425084},
         {-57796140, 27725621},  {-58510273, 28592781},  {-65026204, 36326237},  {-66321141, 37899688},
         {-67553055, 39431322},  {-68707652, 40912081},  {-69414421, 41847539},  {-70373648, 43171709},
         {-70802160, 43801970},  {-69626708, 44423826},  {-69500832, 44580612},  {-69517438, 44759180},
         {-70402232, 46571248},  {-70631353, 47139129},  {-70790322, 47645204},  {-70880079, 48170004},
         {-70836022, 48432201},  {-70393757, 48597581},  {-69696951, 48717065},  {-69166672, 48746854},
         {-66168401, 48719007},  {-66004571, 48777066},  {-65913199, 48953792},  {-65819709, 50214929},
         {-64567977, 49966674},  {-63318168, 49705469},  {-60009943, 48909169},  {-56515788, 47981280},
         {-54126539, 47316536},  {-46386391, 45110400},  {-43369296, 44277479},  {-40263700, 43467720},
         {-39395835, 43264181},  {-37625205, 42849082},  {-37483166, 42819432},  {-36253801, 42563516},
         {-35674412, 42454458},  {-35515136, 42424491},  {-35048870, 42199191},  {-34862709, 42168781},
         {-33252621, 41926411},  {-32502942, 41835599},  {-31999592, 41778303},  {-31076021, 41691629},
         {-30193707, 41636746},  {-29260187, 41590640},  {-29176144, 41589180},  {-28142088, 41581326},
         {-27548623, 41596261},  {-26950500, 41621514},  {-26907420, 41624187},  {-27296983, 41112633},
         {-27381326, 40996047},  {-27989012, 40963451},  {-28138692, 40959253},  {-29172601, 40940110},
         {-30216723, 40958086},  {-30968347, 40990968},  {-32059596, 41069576},  {-32574047, 41111839},
         {-33323922, 41188523},  {-33355502, 41192102},  {-34970203, 41401547},  {-35176124, 41432378},
         {-35690171, 41369764},  {-36438808, 41490323},  {-37698617, 41699347},  {-39653744, 42065692},
         {-43800396, 42915182},  {-45342457, 43261526},  {-45348345, 43262775},  {-50568599, 44305692},
         {-50574460, 44306791},  {-53615613, 44840310},  {-53623507, 44841566},  {-55534335, 45114997},
         {-56455716, 45222015},  {-56990415, 45265339},  {-58176151, 45361070},  {-58988986, 45390201},
         {-59754351, 45388396},  {-60364211, 45358767},  {-61360217, 45251837},  {-62159687, 45076387},
         {-62794134, 44850846},  {-63424043, 44497175},  {-63912607, 44054027},  {-64275381, 43487793},
         {-64498459, 42717071},  {-64535148, 42192268},  {-64471205, 41405650},  {-64314543, 40690545},
         {-64120100, 40058090},  {-63874462, 39404122},  {-63581000, 38726318},  {-63242248, 38023061},
         {-63048962, 37665614},  {-62451850, 36563209},  {-61998277, 35793781},  {-61994661, 35787838},
         {-61010738, 34219433},  {-61006329, 34212647},  {-59353428, 31755703},  {-58915997, 31155400},
         {-58904968, 31139811},  {-58173450, 30074020},  {-57605465, 29308774},  {-57267309, 28853350},
         {-56935597, 28379741},  {-56758677, 27893678},  {-55833774, 26626414},  {-55384145, 26031982},
         {-55378724, 26024436},  {-54620031, 24974408},  {-54614601, 24966864},  {-53686336, 23672495},
         {-53263954, 23077836},  {-52819950, 22424397},  {-52812898, 22413958},  {-52039562, 21262463},
         {-51720551, 20779964},  {-51268827, 20062375},  {-51004463, 19621601},  {-50993923, 19603829},
         {-50450501, 18677012},  {-50134549, 18108099},  {-49637035, 17140780},  {-49202252, 16226805},
         {-48915776, 15586329},  {-48502177, 14536860},  {-48293881, 13926867},  {-48131171, 13424218},
         {-47957911, 12802996},  {-47710562, 11741154},  {-47547264, 10744476},  {-47411827, 9350391},
         {-47399989, 9163312},   {-47526019, 8468044},   {-47381692, 7839505},   {-47527604, 7621018},
         {-47487457, 5279674},   {-47644915, 4762661},   {-47645107, 4755332},   {-47560209, 1681367},
         {-47560253, 1679345},   {-47591199, 565817},    {-47610875, 84616},     {-47689099, -976457},
         {-47742308, -1576171},  {-47842045, -2389891},  {-47996471, -3333559},  {-48219314, -4396851},
         {-48438557, -5270192},  {-48712153, -6237043},  {-49060161, -7284025},  {-49063021, -7292452},
         {-49415948, -8239818},  {-49473812, -8389762},  {-49953551, -9530354},  {-50412680, -10529202},
         {-50415227, -10534771}, {-50893546, -11521628}, {-50916067, -11567425}, {-51554754, -12808303},
         {-52019235, -13695926}, {-52446210, -14467999}, {-52801876, -15142704}, {-52808818, -15155800},
         {-53480037, -16413921}, {-53584936, -16919960}, {-53843700, -17435906}, {-53846126, -17440877},
         {-54868733, -19595920}, {-54872166, -19603470}, {-55326937, -20648845}, {-55543581, -21190124},
         {-55680915, -21533885}, {-56036127, -22524045}, {-56325917, -23486648}, {-56536000, -24407114},
         {-56625601, -25224967}, {-56678029, -25738177}, {-56653328, -26373647}, {-56547823, -26988562},
         {-56342934, -27593803}, {-56040970, -28128453}, {-55803719, -28427693}, {-55381973, -28822864},
         {-54839359, -29182948}, {-54386222, -29398844}, {-53567537, -29700868}, {-52853584, -29879786},
         {-52093673, -30015999}, {-51292005, -30115523}, {-50452932, -30183406}, {-49578991, -30224183},
         {-49569346, -30224448}, {-47759328, -30239656}, {-47749986, -30239561}, {-45372177, -30171922},
         {-44666915, -30125409}, {-43916922, -30103842}, {-43639264, -30083267}, {-43130808, -30180062},
         {-42674180, -30139307}, {-41463591, -30058983}, {-41449327, -30058073}, {-39193983, -29919875},
         {-39155994, -29917901}, {-38098825, -29878010}, {-38081454, -29877418}, {-36709714, -29835798},
         {-36306241, -29836742}, {-34937704, -29853628}, {-34254369, -29892783}, {-34238932, -29893784},
         {-33203711, -29965009}, {-32164627, -30082538}, {-31146078, -30236999}, {-30227180, -30411554},
         {-29766902, -30710288}});

}
