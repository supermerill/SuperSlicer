#ifndef libslic3r_Subdivide_hpp_
#define libslic3r_Subdivide_hpp_

#include "TriangleMesh.hpp"

namespace Slic3r {

indexed_triangle_set its_subdivide(const indexed_triangle_set &its, float max_length);

}

#endif //libslic3r_Subdivide_hpp_
