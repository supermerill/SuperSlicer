///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_DataTreeFwd_hpp_
#define slic3r_DataTreeFwd_hpp_

#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <tcbspan/span.hpp>

#include "PluginProperty.hpp"

// Forward declarations for the main data tree types.
// Include this file from headers that only store pointers, references or simple
// pointer containers to these types, so they do not pull the full model/print
// headers and their transitive dependencies.

namespace Slic3r {

// base types
class Print;
class Model;
class ModelInstance;
class ModelMaterial;
class ModelObject;
class ModelVolume;
class PrintRegion;
class PrintObject;
class PrintObjectRegions;
struct PrintInstance;
class Layer;
class LayerRegion;
class LayerSliceIsland;
class LayerRegionIsland;

// vector definition for shortness
using LayerUPtr = std::unique_ptr<Layer>;
using LayerUPtrs = std::vector<std::unique_ptr<Layer>>;
using LayerRegionSetCPtrs = std::set<const LayerRegion*>;
using LayerRegionUPtr = std::unique_ptr<LayerRegion>;
using LayerRegionUPtrs = std::vector<LayerRegionUPtr>;
using LayerRegionIslandUPtr = std::unique_ptr<LayerRegionIsland>;
using LayerRegionIslandUPtrs = std::vector<LayerRegionIslandUPtr>;
using LayerSliceIslandUPtr = std::unique_ptr<LayerSliceIsland>;
using LayerSliceIslandUPtrs = std::vector<LayerSliceIslandUPtr>;
using ModelInstancePtrs = std::vector<ModelInstance*>;
using ModelObjectPtrs = std::vector<ModelObject*>;
using ConstModelObjectPtrs = std::vector<const ModelObject*>;
using ModelObjectUPtr = std::unique_ptr<ModelObject>;
using ModelObjectUPtrs = std::vector<ModelObjectUPtr>;
using ModelVolumePtrs = std::vector<ModelVolume*>;
using PrintObjectUPtr = std::unique_ptr<PrintObject>;
using PrintObjectUPtrs = std::vector<PrintObjectUPtr>;
using PrintObjectPtrs = std::vector<PrintObject*>;
using PrintInstances = std::vector<PrintInstance>;
using PrintObjectRegionsPtr = std::shared_ptr<PrintObjectRegions>;
using PrintRegionUPtr = std::unique_ptr<PrintRegion>;
using PrintRegionUPtrs = std::vector<PrintRegionUPtr>;
using PrintRegionPtrs = std::vector<PrintRegion*>;

template<class T>
using SpanOfConstPtrs = tcb::span<const T* const>;

// Lightweight non-owning view over a container of pointer-like objects, such as
// std::vector<std::unique_ptr<T>>. It exposes references to the pointed objects:
//
//     std::vector<std::unique_ptr<Layer>> layers;
//     RefView<Layer, decltype(layers)> view(layers);
//     Layer       &layer0 = view[0];
//     const Layer &first  = make_ref_view<Layer>(layers).front();
//
// Use this when the container owns objects through pointers, but callers should
// consume the objects as mandatory, non-null references. All entries must be
// non-null, and the returned view must not outlive the underlying container.
template<class T, class PtrContainer>
class RefView
{
public:
    using container_type = PtrContainer;
    using size_type      = typename container_type::size_type;
    using difference_type = typename container_type::difference_type;
    using value_type     = T;
    using reference      = std::conditional_t<std::is_const<container_type>::value, const T&, T&>;
    using pointer        = std::conditional_t<std::is_const<container_type>::value, const T*, T*>;

    class iterator
    {
    public:
        using base_iterator     = decltype(std::declval<container_type&>().begin());
        using iterator_category = std::random_access_iterator_tag;
        using difference_type   = typename RefView::difference_type;
        using value_type        = typename RefView::value_type;
        using reference         = typename RefView::reference;
        using pointer           = typename RefView::pointer;

        iterator() = default;
        explicit iterator(base_iterator it) : m_it(it) {}

        reference operator*() const { return **m_it; }
        pointer operator->() const { return std::addressof(**m_it); }
        typename RefView::reference operator[](difference_type n) const { return **(m_it + n); }

        iterator& operator++() { ++ m_it; return *this; }
        iterator operator++(int) { iterator out = *this; ++ *this; return out; }
        iterator& operator--() { -- m_it; return *this; }
        iterator operator--(int) { iterator out = *this; -- *this; return out; }

        iterator& operator+=(difference_type n) { m_it += n; return *this; }
        iterator& operator-=(difference_type n) { m_it -= n; return *this; }

        friend iterator operator+(iterator it, difference_type n) { it += n; return it; }
        friend iterator operator+(difference_type n, iterator it) { it += n; return it; }
        friend iterator operator-(iterator it, difference_type n) { it -= n; return it; }
        friend difference_type operator-(const iterator &lhs, const iterator &rhs) { return lhs.m_it - rhs.m_it; }

        friend bool operator==(const iterator &lhs, const iterator &rhs) { return lhs.m_it == rhs.m_it; }
        friend bool operator!=(const iterator &lhs, const iterator &rhs) { return lhs.m_it != rhs.m_it; }
        friend bool operator< (const iterator &lhs, const iterator &rhs) { return lhs.m_it <  rhs.m_it; }
        friend bool operator> (const iterator &lhs, const iterator &rhs) { return rhs < lhs; }
        friend bool operator<=(const iterator &lhs, const iterator &rhs) { return ! (rhs < lhs); }
        friend bool operator>=(const iterator &lhs, const iterator &rhs) { return ! (lhs < rhs); }

    private:
        base_iterator m_it;
    };

    using reverse_iterator = std::reverse_iterator<iterator>;

    explicit RefView(container_type &items) : m_items(items) {}

    size_type size() const { return m_items.size(); }
    bool empty() const { return m_items.empty(); }

    reference operator[](size_type idx) const { return *m_items[idx]; }
    reference at(size_type idx) const { return *m_items.at(idx); }
    reference front() const { return *m_items.front(); }
    reference back() const { return *m_items.back(); }

    iterator begin() const { return iterator(m_items.begin()); }
    iterator end() const { return iterator(m_items.end()); }
    reverse_iterator rbegin() const { return reverse_iterator(end()); }
    reverse_iterator rend() const { return reverse_iterator(begin()); }

private:
    container_type &m_items;
};

template<class T, class PtrContainer>
RefView<T, PtrContainer> make_ref_view(PtrContainer &items)
{
    return RefView<T, PtrContainer>(items);
}

template<class T, class PtrContainer>
RefView<T, const PtrContainer> make_ref_view(const PtrContainer &items)
{
    return RefView<T, const PtrContainer>(items);
}

using LayerRefs = RefView<Layer, LayerUPtrs>;
using LayerCRefs = RefView<Layer, const LayerUPtrs>;
using ModelObjectRefs = RefView<ModelObject, ModelObjectUPtrs>;
using ModelObjectCRefs = RefView<ModelObject, const ModelObjectUPtrs>;
using LayerRegionRefs = RefView<LayerRegion, LayerRegionUPtrs>;
using LayerRegionCRefs = RefView<LayerRegion, const LayerRegionUPtrs>;
using LayerSliceIslandRefs = RefView<LayerSliceIsland, LayerSliceIslandUPtrs>;
using LayerSliceIslandCRefs = RefView<LayerSliceIsland, const LayerSliceIslandUPtrs>;
using LayerRegionIslandRefs = RefView<LayerRegionIsland, LayerRegionIslandUPtrs>;
using LayerRegionIslandCRefs = RefView<LayerRegionIsland, const LayerRegionIslandUPtrs>;

using PrintObjectCRefs = RefView<PrintObject, const PrintObjectUPtrs>;
using PrintObjectRefs = RefView<PrintObject, PrintObjectUPtrs>;
using PrintRegionCRefs = RefView<PrintRegion, const PrintRegionPtrs>;


} // namespace Slic3r

#endif // slic3r_DataTreeFwd_hpp_
