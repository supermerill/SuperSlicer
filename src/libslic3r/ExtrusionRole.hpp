///|/ Copyright (c) 2023 Robert Schiele @schiele
///|/ Copyright (c) Prusa Research 2023 Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ExtrusionRole_hpp_
#define slic3r_ExtrusionRole_hpp_

#include "enum_bitmask.hpp"

#include <string>
#include <string_view>
#include <cstdint>

namespace Slic3r {
//that's good and clean but a pain in the ass to debug with the debuggeur.
//
//enum class ExtrusionRoleModifier : uint16_t {
//    // 1) Extrusion types
//    // Perimeter (external, inner, ...)
//    Perimeter,
//    // Infill (top / bottom / solid inner / sparse inner / bridging inner ...)
//    Infill,
//    // Support material extrusion
//    Support,
//    Skirt, //(brim if not external)
//    Wipe,
//    Mill,
//    // 2) Extrusion modifiers
//    External,
//    Solid,
//    Ironing,
//    Bridge,
//    // Variable width extrusion (also gapfill or thinwall if external)
//    Thin,
//    // 3) Special types
//    // Indicator that the extrusion role was mixed from multiple differing extrusion roles,
//    // for example from Support and SupportInterface.
//    Mixed,
//    //Travel
//    Travel,
//    // Stopper, there should be maximum 16 modifiers defined for uint16_t bit mask.
//    Count
//};
//// There should be maximum 16 modifiers defined for uint16_t bit mask.
//static_assert(int(ExtrusionRoleModifier::Count) <= 16,
//              "ExtrusionRoleModifier: there must be maximum 16 modifiers defined to fit a 16 bit bitmask");
//
//using ExtrusionRoleModifiers = enum_bitmask<ExtrusionRoleModifier>;
//ENABLE_ENUM_BITMASK_OPERATORS(ExtrusionRoleModifier);
//
//struct ExtrusionRole : public ExtrusionRoleModifiers
//{
//    constexpr ExtrusionRole(const ExtrusionRoleModifier bit) : ExtrusionRoleModifiers(bit) {}
//    constexpr ExtrusionRole(const ExtrusionRoleModifiers bits) : ExtrusionRoleModifiers(bits) {}
//
//    static constexpr const ExtrusionRoleModifiers None{};
//    // Internal perimeter, not bridging.
//    static constexpr const ExtrusionRoleModifiers Perimeter{ExtrusionRoleModifier::Perimeter};
//    // External perimeter, not bridging.
//    static constexpr const ExtrusionRoleModifiers ExternalPerimeter{ExtrusionRoleModifier::Perimeter | ExtrusionRoleModifier::External};
//    // Perimeter, bridging. To be or'ed with ExtrusionRoleModifier::External for external bridging perimeter.
//    static constexpr const ExtrusionRoleModifiers OverhangPerimeter{ExtrusionRoleModifier::Perimeter | ExtrusionRoleModifier::Bridge};
//    // Sparse internal infill.
//    static constexpr const ExtrusionRoleModifiers InternalInfill{ExtrusionRoleModifier::Infill};
//    // Solid internal infill.
//    static constexpr const ExtrusionRoleModifiers SolidInfill{ExtrusionRoleModifier::Infill | ExtrusionRoleModifier::Solid};
//    // Top solid infill (visible).
//    // FIXME why there is no bottom solid infill type?
//    static constexpr const ExtrusionRoleModifiers TopSolidInfill{ExtrusionRoleModifier::Infill | ExtrusionRoleModifier::Solid |
//                                                                 ExtrusionRoleModifier::External};
//    // Ironing infill at the top surfaces.
//    static constexpr const ExtrusionRoleModifiers Ironing{ExtrusionRoleModifier::Infill | ExtrusionRoleModifier::Solid |
//                                                          ExtrusionRoleModifier::Ironing | ExtrusionRoleModifier::External};
//    // Visible bridging infill at the bottom of an object.
//    static constexpr const ExtrusionRoleModifiers BridgeInfill{ExtrusionRoleModifier::Infill | ExtrusionRoleModifier::Solid |
//                                                               ExtrusionRoleModifier::Bridge | ExtrusionRoleModifier::External};
//    static constexpr const ExtrusionRoleModifiers InternalBridgeInfill{ExtrusionRoleModifier::Infill | ExtrusionRoleModifier::Solid |
//                                                                       ExtrusionRoleModifier::Bridge};
//    // Gap fill extrusion, currently used for any variable width extrusion: Thin walls outside of the outer extrusion,
//    // gap fill in between perimeters, gap fill between the inner perimeter and infill.
//    static constexpr const ExtrusionRoleModifiers GapFill{ExtrusionRoleModifier::Thin};
//    static constexpr const ExtrusionRoleModifiers ThinWall{ExtrusionRoleModifier::Thin | ExtrusionRoleModifier::External};
//    static constexpr const ExtrusionRoleModifiers Skirt{ExtrusionRoleModifier::Skirt};
//    // Support base material, printed with non-soluble plastic.
//    static constexpr const ExtrusionRoleModifiers SupportMaterial{ExtrusionRoleModifier::Support};
//    // Support interface material, printed with soluble plastic.
//    static constexpr const ExtrusionRoleModifiers SupportMaterialInterface{ExtrusionRoleModifier::Support | ExtrusionRoleModifier::External};
//    // Wipe tower material.
//    static constexpr const ExtrusionRoleModifiers WipeTower{ExtrusionRoleModifier::Wipe};
//    // Milling
//    static constexpr const ExtrusionRoleModifiers Milling{ExtrusionRoleModifier::Mill};
//    // Extrusion role for a collection with multiple extrusion roles.
//    static constexpr const ExtrusionRoleModifiers Mixed{ExtrusionRoleModifier::Mixed};
//    // Travel
//    static constexpr const ExtrusionRoleModifiers Travel{ExtrusionRoleModifier::Travel};
//
//    bool is_perimeter() const { return this->ExtrusionRoleModifiers::has(ExtrusionRoleModifier::Perimeter); }
//    bool is_external_perimeter() const { return this->is_perimeter() && this->is_external(); }
//    bool is_infill() const { return this->ExtrusionRoleModifiers::has(ExtrusionRoleModifier::Infill); }
//    bool is_solid_infill() const { return this->is_infill() && this->ExtrusionRoleModifiers::has(ExtrusionRoleModifier::Solid); }
//    bool is_sparse_infill() const { return this->is_infill() && !this->ExtrusionRoleModifiers::has(ExtrusionRoleModifier::Solid); }
//    bool is_external() const { return this->ExtrusionRoleModifiers::has(ExtrusionRoleModifier::External); }
//    bool is_bridge() const { return this->ExtrusionRoleModifiers::has(ExtrusionRoleModifier::Bridge); }
//
//    bool is_support() const { return this->ExtrusionRoleModifiers::has(ExtrusionRoleModifier::Support); }
//    bool is_support_base() const { return this->is_support() && !this->is_external(); }
//    bool is_support_interface() const { return this->is_support() && this->is_external(); }
//    bool is_mixed() const { return this->ExtrusionRoleModifiers::has(ExtrusionRoleModifier::Mixed); }
//
//    // Brim is currently marked as skirt.
//    bool is_skirt() const { return this->ExtrusionRoleModifiers::has(ExtrusionRoleModifier::Skirt); }
//};

//easy to debug ExtrusionRoles
enum ExtrusionRoleModifier : uint16_t {
    // 1) Extrusion types
    // Perimeter (external, inner, ...)
    ERM_Perimeter = 1 << 0, //1
    // Infill (top / bottom / solid inner / sparse inner / bridging inner ...)
    ERM_Infill = 1 << 1, //2
    // Support material extrusion
    ERM_Support = 1 << 2, //4
    ERM_Skirt = 1 << 3, //8 //(brim if not external)
    ERM_Wipe = 1 << 4, //16
    ERM_Mill = 1 << 5, //32
    // 2) Extrusion modifiers
    ERM_External = 1 << 6, //64
    ERM_Solid = 1 << 7, //128
    ERM_Ironing = 1 << 8, //256
    ERM_Bridge = 1 << 9, //512
    // Variable width extrusion (also gapfill or thinwall if external)
    ERM_Thin = 1 << 10, //1024
    // 3) Special types
    // Indicator that the extrusion role was mixed from multiple differing extrusion roles,
    // for example from Support and SupportInterface.
    ERM_Mixed = 1 << 11, //2048
    //Travel
    ERM_Travel = 1 << 12, //4096
    // Stopper, there should be maximum 16 modifiers defined for uint16_t bit mask.
    //Count
};
constexpr ExtrusionRoleModifier operator|(ExtrusionRoleModifier a, ExtrusionRoleModifier b) {
    return static_cast<ExtrusionRoleModifier>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
constexpr ExtrusionRoleModifier operator&(ExtrusionRoleModifier a, ExtrusionRoleModifier b) {
    return static_cast<ExtrusionRoleModifier>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
constexpr ExtrusionRoleModifier operator^(ExtrusionRoleModifier a, ExtrusionRoleModifier b) {
    return static_cast<ExtrusionRoleModifier>(static_cast<uint16_t>(a) ^ static_cast<uint16_t>(b));
}
constexpr ExtrusionRoleModifier operator|=(ExtrusionRoleModifier& a, ExtrusionRoleModifier b) {
    a = a | b; return a;
}
constexpr ExtrusionRoleModifier operator&=(ExtrusionRoleModifier& a, ExtrusionRoleModifier b) {
    a = a & b; return a;
}
static inline bool is_perimeter(ExtrusionRoleModifier role) { return (role & ExtrusionRoleModifier::ERM_Perimeter); }
static inline bool is_external(ExtrusionRoleModifier role) { return (role & ExtrusionRoleModifier::ERM_External); }
static inline bool is_bridge(ExtrusionRoleModifier role) { return (role & ExtrusionRoleModifier::ERM_Bridge); }
static inline bool is_external_perimeter(ExtrusionRoleModifier role) { return is_perimeter(role) && is_external(role); }
static inline bool is_infill(ExtrusionRoleModifier role) { return (role & ExtrusionRoleModifier::ERM_Infill); }
static inline bool is_solid_infill(ExtrusionRoleModifier role) {
    return is_infill(role) && (role & ExtrusionRoleModifier::ERM_Solid);
}
static inline bool is_sparse_infill(ExtrusionRoleModifier role) {
    return is_infill(role) && !(role & ExtrusionRoleModifier::ERM_Solid);
}

static inline bool is_support(ExtrusionRoleModifier role) { return (role & ExtrusionRoleModifier::ERM_Support); }
static inline bool is_support_base(ExtrusionRoleModifier role) { return is_support(role) && !is_external(role); }
static inline bool is_support_interface(ExtrusionRoleModifier role) { return is_support(role) && is_external(role); }
static inline bool is_mixed(ExtrusionRoleModifier role) { return (role & ExtrusionRoleModifier::ERM_Mixed); }

// Brim is currently marked as skirt.
static inline bool is_skirt(ExtrusionRoleModifier role) { return (role & ExtrusionRoleModifier::ERM_Skirt); }
// composition to keep the nice debugguable ExtrusionRoleModifier
struct ExtrusionRole
{
private:
    ExtrusionRoleModifier m;
    static constexpr const ExtrusionRoleModifier BaseType{ERM_Perimeter | ERM_Infill | ERM_Support | ERM_Skirt | ERM_Wipe | ERM_Mill | ERM_Travel | ERM_Thin | ERM_Mixed};
public:
    ExtrusionRole() = delete;
    ExtrusionRole(ExtrusionRoleModifier mods) : m(mods) {
        //assert(m != 0);
        // check at least one base type
        assert(!m || (m & ExtrusionRole::BaseType));
        // check only one base type (pow 2)
        assert( ((m & ExtrusionRole::BaseType) & ((m & ExtrusionRole::BaseType) - 1)) == 0);
    }

    //modifiers
    static constexpr const ExtrusionRoleModifier Bridge{ExtrusionRoleModifier::ERM_Bridge};

    static constexpr const ExtrusionRoleModifier None{};
    // Internal perimeter, not bridging.
    static constexpr const ExtrusionRoleModifier Perimeter{ExtrusionRoleModifier::ERM_Perimeter};
    // External perimeter, not bridging.
    static constexpr const ExtrusionRoleModifier ExternalPerimeter{ExtrusionRoleModifier::ERM_Perimeter |
                                                                   ExtrusionRoleModifier::ERM_External};

    // Perimeter, bridging. To be or'ed with ExtrusionRoleModifier::External for external bridging perimeter.
    static constexpr const ExtrusionRoleModifier OverhangPerimeter{ExtrusionRoleModifier::ERM_Perimeter |
                                                                   ExtrusionRoleModifier::ERM_Bridge};
    static constexpr const ExtrusionRoleModifier OverhangExternalPerimeter{ExtrusionRoleModifier::ERM_Perimeter |
                                                                   ExtrusionRoleModifier::ERM_External |
                                                                   ExtrusionRoleModifier::ERM_Bridge};
    // Sparse internal infill.
    static constexpr const ExtrusionRoleModifier InternalInfill{ExtrusionRoleModifier::ERM_Infill};
    // Solid internal infill.
    static constexpr const ExtrusionRoleModifier SolidInfill{ExtrusionRoleModifier::ERM_Infill |
                                                             ExtrusionRoleModifier::ERM_Solid};
    // Top solid infill (visible).
    // FIXME why there is no bottom solid infill type?
    static constexpr const ExtrusionRoleModifier TopSolidInfill{
        ExtrusionRoleModifier::ERM_Infill | ExtrusionRoleModifier::ERM_Solid | ExtrusionRoleModifier::ERM_External};
    // Ironing infill at the top surfaces.
    static constexpr const ExtrusionRoleModifier Ironing{
        ExtrusionRoleModifier::ERM_Infill | ExtrusionRoleModifier::ERM_Solid | ExtrusionRoleModifier::ERM_Ironing |
        ExtrusionRoleModifier::ERM_External};
    // Visible bridging infill at the bottom of an object.
    static constexpr const ExtrusionRoleModifier BridgeInfill{
        ExtrusionRoleModifier::ERM_Infill | ExtrusionRoleModifier::ERM_Solid | ExtrusionRoleModifier::ERM_Bridge |
        ExtrusionRoleModifier::ERM_External};
    static constexpr const ExtrusionRoleModifier InternalBridgeInfill{
        ExtrusionRoleModifier::ERM_Infill | ExtrusionRoleModifier::ERM_Solid | ExtrusionRoleModifier::ERM_Bridge};
    // Gap fill extrusion, currently used for any variable width extrusion: Thin walls outside of the outer extrusion,
    // gap fill in between perimeters, gap fill between the inner perimeter and infill.
    static constexpr const ExtrusionRoleModifier GapFill{ExtrusionRoleModifier::ERM_Thin};
    static constexpr const ExtrusionRoleModifier ThinWall{ExtrusionRoleModifier::ERM_Thin |
                                                          ExtrusionRoleModifier::ERM_External};
    static constexpr const ExtrusionRoleModifier Skirt{ExtrusionRoleModifier::ERM_Skirt};
    // Support base material, printed with non-soluble plastic.
    static constexpr const ExtrusionRoleModifier SupportMaterial{ExtrusionRoleModifier::ERM_Support};
    // Support interface material, printed with soluble plastic.
    static constexpr const ExtrusionRoleModifier SupportMaterialInterface{ExtrusionRoleModifier::ERM_Support |
                                                                          ExtrusionRoleModifier::ERM_External};
    // Wipe tower material.
    static constexpr const ExtrusionRoleModifier WipeTower{ExtrusionRoleModifier::ERM_Wipe};
    // Milling
    static constexpr const ExtrusionRoleModifier Milling{ExtrusionRoleModifier::ERM_Mill};
    // Extrusion role for a collection with multiple extrusion roles.
    static constexpr const ExtrusionRoleModifier Mixed{ExtrusionRoleModifier::ERM_Mixed};
    // Travel
    static constexpr const ExtrusionRoleModifier Travel{ExtrusionRoleModifier::ERM_Travel};
    
    bool is_perimeter() const { return (m & ExtrusionRoleModifier::ERM_Perimeter); }
    bool is_external_perimeter() const { return this->is_perimeter() && this->is_external(); }
    bool is_overhang() const { return (m & (ExtrusionRoleModifier::ERM_Perimeter | ExtrusionRoleModifier::ERM_Bridge)); }
    bool is_infill() const { return (m & ExtrusionRoleModifier::ERM_Infill); }
    bool is_solid_infill() const { return this->is_infill() && (m & ExtrusionRoleModifier::ERM_Solid); }
    bool is_sparse_infill() const { return this->is_infill() && !(m & ExtrusionRoleModifier::ERM_Solid); }
    bool is_external() const { return (m & ExtrusionRoleModifier::ERM_External); }
    bool is_bridge() const { return (m & ExtrusionRoleModifier::ERM_Bridge); }

    bool is_support() const { return (m & ExtrusionRoleModifier::ERM_Support); }
    bool is_support_base() const { return this->is_support() && !this->is_external(); }
    bool is_support_interface() const { return this->is_support() && this->is_external(); }
    bool is_mixed() const { return (m & ExtrusionRoleModifier::ERM_Mixed); }

    // Brim is currently marked as skirt.
    bool is_skirt() const { return (m & ExtrusionRoleModifier::ERM_Skirt); }

    
    ExtrusionRole operator|(ExtrusionRole b) const { return ExtrusionRole(this->m | b.m); }
    ExtrusionRole operator&(ExtrusionRole b) const { return ExtrusionRole(this->m & b.m); }
    ExtrusionRole operator^(ExtrusionRole b) const { return ExtrusionRole(this->m ^ b.m); }
    ExtrusionRole operator|(ExtrusionRoleModifier b) const { return ExtrusionRole(this->m | b); }
    ExtrusionRole operator&(ExtrusionRoleModifier b) const { return ExtrusionRole(this->m & b); }
    ExtrusionRole operator^(ExtrusionRoleModifier b) const { return ExtrusionRole(this->m ^ b); }
    ExtrusionRole& operator|=(ExtrusionRole b) { m = m | b.m; return *this; }
    ExtrusionRole& operator&=(ExtrusionRole b) { m = m & b.m; return *this; }
    bool operator==(ExtrusionRole const &rhs) const { return m == rhs.m; }
    bool operator==(ExtrusionRoleModifier const &rhs) const { return m == rhs; }
    bool operator!=(ExtrusionRole const &rhs) const { return m != rhs.m; }
    bool operator!=(ExtrusionRoleModifier const &rhs) const { return m != rhs; }
    bool operator<( ExtrusionRole const & rhs ) const { return m < rhs.m; }

    // like operators but not operators
    bool has(ExtrusionRoleModifier r) const { return m & r; }
    
    // create ambiguity. need explicit
    explicit operator ExtrusionRoleModifier() const { return m; }

    //to easily get the ExtrusionRoleModifier
    ExtrusionRoleModifier operator()() const { return m; }
};

// creates an error, as they are duplicated in multiple obj & dll, i don't know why.
//bool operator==(ExtrusionRoleModifier const &lhs, ExtrusionRole const &rhs);
//bool operator!=(ExtrusionRoleModifier const &lhs, ExtrusionRole const &rhs);
//ExtrusionRole operator|(ExtrusionRoleModifier const &lhs, ExtrusionRole const &rhs);
//ExtrusionRole operator&(ExtrusionRoleModifier const &lhs, ExtrusionRole const &rhs);
//ExtrusionRole operator^(ExtrusionRoleModifier const &lhs, ExtrusionRole const &rhs);

// Special flags describing loop
enum ExtrusionLoopRole : uint16_t {
    // useless
    elrDefault = 1 << 0, // 1
    // doesn't contains more contour: it's the most internal one
    elrInternal = 1 << 1, // 2
    elrSkirt    = 1 << 2, // 4
    // it's a modifier that indicate that the loop is around a hole, not around the infill
    elrHole = 1 << 3, // 8
    // it's a modifier that indicate that the loop should be printed as vase
    elrVase = 1 << 4, // 16
    // it's a modifier that indicate that the loop does not contains an inner loop, used for random seam
    elrFirstLoop = 1 << 5, // 32
};

// Be careful when editing this list, you also have to add values to other lists like
// GCodeViewer::Extrusion_Role_Colors ; for that, search occurences of GCodeExtrusionRole::Custom
enum class GCodeExtrusionRole : uint8_t {
    None = 0,
    Perimeter,
    ExternalPerimeter,
    OverhangPerimeter,
    InternalInfill,
    InternalBridgeInfill,
    SolidInfill,
    TopSolidInfill,
    Ironing,
    BridgeInfill,
    ThinWall,
    GapFill,
    Skirt,
    SupportMaterial,
    SupportMaterialInterface,
    WipeTower,
    Milling,
    // Custom (user defined) G-code block, for example start / end G-code.
    Custom,
    // for post-processing
    Travel,
    // Stopper to count number of enums.
    Count
};

// Convert a rich bitmask based ExtrusionRole to a less expressive ordinal GCodeExtrusionRole.
// GCodeExtrusionRole is to be serialized into G-code and deserialized by G-code viewer,
GCodeExtrusionRole extrusion_role_to_gcode_extrusion_role(ExtrusionRole role);

std::string        gcode_extrusion_role_to_string(GCodeExtrusionRole role);
GCodeExtrusionRole string_to_gcode_extrusion_role(const std::string_view role);

//for debug output
std::string role_to_code(ExtrusionRole role);
std::string looprole_to_code(ExtrusionLoopRole role);

inline std::string er_to_string(ExtrusionRole role) { return gcode_extrusion_role_to_string(extrusion_role_to_gcode_extrusion_role(role)); }

} // namespace Slic3r

//for unordered_set/map
namespace std {
template<> struct hash<Slic3r::ExtrusionRole>
{
    typedef Slic3r::ExtrusionRole argument_type;
    typedef uint16_t              result_type;

    result_type operator()(const argument_type &t) const {
        return t();
    }
};
} // namespace std

#endif // slic3r_ExtrusionRole_hpp_
