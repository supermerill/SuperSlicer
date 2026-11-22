// cgal_compat.hpp - Full compatibility for upstream PR
#pragma once

#include <CGAL/config.h>
#include <type_traits>

// Detect CGAL version to choose appropriate headers
#define CGAL_VERSION_AT_LEAST(major, minor, patch) \
    (CGAL_VERSION_NR >= ((major) * 100000000 + (minor) * 1000000 + (patch) * 10000))

// Include appropriate AABB header based on availability
// CGAL 5.0+ uses AABB_traits_3, older versions use AABB_traits
#if CGAL_VERSION_AT_LEAST(5, 0, 0) || __has_include(<CGAL/AABB_traits_3.h>)
    #include <CGAL/AABB_traits_3.h>
    #define CGAL_HAS_MODERN_AABB
#else
    // Only include deprecated header if modern one isn't available
    #include <CGAL/AABB_traits.h>
    #define CGAL_HAS_LEGACY_AABB
#endif

namespace cgal_compat_detail {
    // SFINAE helper to detect std::optional-like API (CGAL 5.4+)
    template<typename T>
    auto test_optional_api(int) -> decltype(std::declval<T>().value(), std::true_type{});
    
    template<typename T>
    auto test_optional_api(...) -> std::false_type;
    
    template<typename T>
    constexpr bool has_optional_api_v = decltype(test_optional_api<T>(0))::value;
    
    // SFINAE helper to detect std::pair-like API (older CGAL)
    template<typename T>
    auto test_pair_api(int) -> decltype(std::declval<T>().first, std::declval<T>().second, std::true_type{});
    
    template<typename T>
    auto test_pair_api(...) -> std::false_type;
    
    template<typename T>
    constexpr bool has_pair_api_v = decltype(test_pair_api<T>(0))::value;
}

// Compatibility wrapper for property_map access
template<typename PropertyMap>
auto access_pmap(PropertyMap&& pm) {
    using T = std::decay_t<PropertyMap>;
    
    if constexpr (cgal_compat_detail::has_optional_api_v<T>) {
        // std::optional-like API (CGAL 5.4+)
        if (!pm) {
            throw std::runtime_error("Property map not found");
        }
        return *pm;
    } else if constexpr (cgal_compat_detail::has_pair_api_v<T>) {
        // std::pair-like API (older CGAL)
        if (!pm.second) {
            throw std::runtime_error("Property map not found");
        }
        return pm.first;
    } else {
        static_assert(sizeof(T) == 0, "Unknown property_map return type");
    }
}

// AABB_traits compatibility - use appropriate version
#ifdef CGAL_HAS_MODERN_AABB
    template<typename Kernel, typename Primitive>
    using AABB_traits_compat = CGAL::AABB_traits_3<Kernel, Primitive>;
#elif defined(CGAL_HAS_LEGACY_AABB)
    template<typename Kernel, typename Primitive>
    using AABB_traits_compat = CGAL::AABB_traits<Kernel, Primitive>;
#else
    #error "No AABB_traits found in CGAL installation"
#endif
