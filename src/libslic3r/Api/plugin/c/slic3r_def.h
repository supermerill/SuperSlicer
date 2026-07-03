///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_def_h_
#define slic3r_def_h_

#include <assert.h>
#include <stdint.h>

/*
Generic property ids shared by every host-side property container.

The numeric id is the fast runtime key stored on objects. Built-in ids are
reserved in this header so host and plugin code agree at compile time.
Plugin-defined ids are allocated by orchestrator_register_property() from the
custom range and are valid only for the orchestrator that registered them.
*/
typedef uint32_t slic3r_property_type;

#define SLIC3R_PROPERTY_TYPE_INVALID                  ((slic3r_property_type)UINT32_MAX)
#define SLIC3R_PROPERTY_TYPE_CUSTOM_BEGIN             ((slic3r_property_type)0x80000000u)

#define SLIC3R_PROPERTY_TYPE_EXTRUSION_ATTRIBUTES      ((slic3r_property_type)1u)
#define SLIC3R_PROPERTY_TYPE_EXTRUSION_SPEED           ((slic3r_property_type)2u)
#define SLIC3R_PROPERTY_TYPE_EXTRUSION_MODIFIER        ((slic3r_property_type)3u)
#define SLIC3R_PROPERTY_TYPE_EXTRUSION_CUSTOM_GCODE    ((slic3r_property_type)4u)
#define SLIC3R_PROPERTY_TYPE_EXTRUSION_SPECIAL_COMMAND ((slic3r_property_type)5u)
#define SLIC3R_PROPERTY_TYPE_EXTRUSION_OVERHANG        ((slic3r_property_type)6u)
#define SLIC3R_PROPERTY_TYPE_EXTRUSION_Z_OFFSET        ((slic3r_property_type)7u)
#define SLIC3R_PROPERTY_TYPE_EXTRUSION_PERIMETER       ((slic3r_property_type)9u)
#define SLIC3R_PROPERTY_TYPE_EXTRUSION_INFILL          ((slic3r_property_type)10u)
#define SLIC3R_PROPERTY_TYPE_LAYER_SUPPORT             ((slic3r_property_type)11u)
#define SLIC3R_PROPERTY_TYPE_LAYER_BRIM                ((slic3r_property_type)12u)
#define SLIC3R_PROPERTY_TYPE_LAYER_ADHESION            ((slic3r_property_type)13u)

#ifdef __cplusplus
#define SLIC3R_CONSTEXPR_INLINE constexpr inline
#define SLIC3R_CONSTEXPR_STATIC constexpr static
#else
#define SLIC3R_CONSTEXPR_INLINE static inline
#define SLIC3R_CONSTEXPR_STATIC static const
#endif

#ifndef COORD_64B
#define COORD_64B 1
#endif

#if !COORD_64B
// Saves around 32% RAM after slicing step, 6.7% after G-code export (tested on PrusaSlicer 2.2.0 final).
typedef int32_t coord_t;
typedef double coordf_t;
typedef double distf_t;
typedef double distsqrf_t;
typedef int32_t coord_index_t;
// to optimise computation by staying in int
typedef int64_t lengthsqr_t;

// Scaling factor for a conversion from coord_t to coordf_t: 2e-15 (0.000030517578125)
// This scaling generates a following fixed point representation with for a 32bit integer:
// max: 65m with 30nm resolution but without any EPSILON (so still 3 times better than the old 64bit scaling)
SLIC3R_CONSTEXPR_STATIC int SCALING_POWER = 15;
SLIC3R_CONSTEXPR_STATIC double SCALING_FACTOR = 0x1p-15;
SLIC3R_CONSTEXPR_STATIC double UNSCALING_FACTOR = 0x1p15;
SLIC3R_CONSTEXPR_STATIC double EPSILON = 0x1p-15;
SLIC3R_CONSTEXPR_STATIC coord_t SCALED_EPSILON = 1;


// Equivalent to val * 2^SCALING_POWER. <==> SLIC3R_CONSTEXPR_INLINE coord_t scale_i(coordf_t val) { return (coord_t) (std::scalbn(val, SCALING_POWER); }
SLIC3R_CONSTEXPR_INLINE coord_t scale_i(coordf_t val) { return (coord_t) (val * UNSCALING_FACTOR); }
// Equivalent to val * 2^SCALING_POWER. <==> SLIC3R_CONSTEXPR_INLINE coordf_t scale_d(coordf_t val) { return (coordf_t) (std::scalbn(val, SCALING_POWER)); }
SLIC3R_CONSTEXPR_INLINE coordf_t scale_d(coordf_t val) { return (coordf_t) (val * UNSCALING_FACTOR); }
//Equivalent to val * 2^-SCALING_POWER. <==> SLIC3R_CONSTEXPR_INLINE coordf_t unscaled(coord_t val) { return (coordf_t) (std::scalbn(coordf_t(val), -SCALING_POWER)); }
SLIC3R_CONSTEXPR_INLINE double unscaled(coord_t val) { return ((double)val) * SCALING_FACTOR; }
SLIC3R_CONSTEXPR_INLINE double unscaled(coordf_t val) { return val * SCALING_FACTOR; }
SLIC3R_CONSTEXPR_INLINE distsqrf_t coord_sqr(coord_t length) { return distf_t(length) * distf_t(length); }
SLIC3R_CONSTEXPR_INLINE coord_index_t coord_index(coord_t coord) { 
    return coord;
}
SLIC3R_CONSTEXPR_INLINE lengthsqr_t coord_int_sqr(coord_t length) { 
    return lengthsqr_t(length) * lengthsqr_t(length);
}
#else
typedef int64_t coord_t;
typedef double coordf_t;
typedef double distf_t;
typedef double distsqrf_t;
typedef int32_t coord_index_t;
typedef uint64_t lengthsqr_t;

// with int64_t we don't have to worry anymore about the size of the int.
SLIC3R_CONSTEXPR_STATIC double SCALING_FACTOR = 0.000001;
SLIC3R_CONSTEXPR_STATIC double UNSCALING_FACTOR = 1000000.; // 1 / SCALING_FACTOR; <- linux has some problem compiling this constexpr
// this epsilon reduce the precision to ~0.1um
//FIXME This epsilon value is used for many non-related purposes:
// For a threshold of a squared Euclidean distance,
// for a trheshold in a difference of radians,
// for a threshold of a cross product of two non-normalized vectors etc.
SLIC3R_CONSTEXPR_STATIC double EPSILON = 1e-4;
SLIC3R_CONSTEXPR_STATIC coord_t SCALED_EPSILON = 100; // coord_t(EPSILON/ SCALING_FACTOR); <- linux has some problem compiling this constexpr

SLIC3R_CONSTEXPR_INLINE coord_t scale_i(coordf_t val) { return (coord_t) (val * UNSCALING_FACTOR); }
SLIC3R_CONSTEXPR_INLINE coordf_t scale_d(coordf_t val) { return (coordf_t) (val * UNSCALING_FACTOR); }
SLIC3R_CONSTEXPR_INLINE double unscaled(coord_t val) { return ((double) val) * SCALING_FACTOR; }
SLIC3R_CONSTEXPR_INLINE double unscaled(coordf_t val) { return val * SCALING_FACTOR; }
SLIC3R_CONSTEXPR_INLINE distsqrf_t coord_sqr(coord_t length) { return distf_t(length) * distf_t(length); }
#define SLIC3R_SQUARE_BIT_REDUCTION 7
#define SLIC3R_SQUARE_BIT_FACTOR (1u << SLIC3R_SQUARE_BIT_REDUCTION)
// lossy square (works only for 2^38 length), by dividing by 128 to remove epsilon (and a bit more)
SLIC3R_CONSTEXPR_INLINE coord_index_t coord_index(coord_t coord) { 
    // <==> assert(abs(coord) < std::pow(2,38));
    assert((int64_t)coord < (1LL << 38) && (int64_t)coord > -(1LL << 38));
    // remove epsilon (/SLIC3R_SQUARE_BIT_FACTOR)
    return coord_index_t(coord / SLIC3R_SQUARE_BIT_FACTOR);
}
SLIC3R_CONSTEXPR_INLINE lengthsqr_t coord_int_sqr(coord_t length) { 
    // <==> assert(abs(length) < std::pow(2,38));
    assert((int64_t)length < (1LL << 38) && (int64_t)length > -(1LL << 38));
    // remove epsilon (/SLIC3R_SQUARE_BIT_FACTOR)
    // as we're computing the norm, we can use abs 
    //lengthsqr_t temp = std::abs(length) >> SQUARE_BIT_REDUCTION;
    //lengthsqr_t temp = (lengthsqr_t)(llabs((int64_t)length) >> SQUARE_BIT_REDUCTION);
    lengthsqr_t temp = (length >= 0) ? (lengthsqr_t)length : (lengthsqr_t)(-length);
    temp = temp >> SLIC3R_SQUARE_BIT_REDUCTION;
    return temp * temp;
}
#endif

SLIC3R_CONSTEXPR_INLINE coord_t scale_to_layer_coord(double z) {
    assert(z < 10000);
    assert(z >= 0);
    coord_t coord_z = scale_i(z + EPSILON / 2);
    coord_z /= SCALED_EPSILON;
    coord_z *= SCALED_EPSILON;
    return coord_z;
}

#endif // slic3r_def_h_
