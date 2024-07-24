#ifndef ARRANGEJOB_HPP
#define ARRANGEJOB_HPP

#include <optional>

#include "Job.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/Print.hpp"

namespace Slic3r {

class ModelInstance;

namespace GUI {

class Plater;

class ArrangeJob : public Job
{
    using ArrangePolygon = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    ArrangePolygons m_selected, m_unselected, m_unprintable;
    std::vector<ModelInstance*> m_unarranged;
    Plater *m_plater;
    bool m_force_prepare_all{false};

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();


    // Prepare the selected and unselected items separately. If nothing is
    // selected, behaves as if everything would be selected.
    void prepare_selected();

    ArrangePolygon get_arrange_poly_(ModelInstance *mi);

public:
    // Prepare all objects on the bed regardless of the selection
    //put on public to be accessed by calibrations
    void prepare_all();

    void prepare();

    void process(Ctl &ctl) override;

    ArrangeJob(bool force_prepare_all = false);

    int status_range() const
    {
        return int(m_selected.size() + m_unprintable.size());
    }

    void finalize(bool canceled, std::exception_ptr &e) override;

    // Enabling this option will have all objects be prepared, even if shift key is not pressed
    void set_force_prepare_all(bool value = true) { m_force_prepare_all = value; }
};

std::optional<arrangement::ArrangePolygon> get_wipe_tower_arrangepoly(const Plater &);

// The gap between logical beds in the x axis expressed in ratio of
// the current bed width.
static const constexpr double LOGICAL_BED_GAP = 1. / 5.;

// Stride between logical beds
double bed_stride(const Plater *plater);

template<class T> struct PtrWrapper
{
    T *ptr;

    explicit PtrWrapper(T *p) : ptr{p} {}

    arrangement::ArrangePolygon get_arrange_polygon() const
    {
        return ptr->get_arrange_polygon();
    }

    void apply_arrange_result(const Vec2d &t, double rot)
    {
        ptr->apply_arrange_result(t, rot);
    }
};

// Set up arrange polygon for a ModelInstance and Wipe tower
template<class T>
arrangement::ArrangePolygon get_arrange_poly(T obj, const Plater *plater)
{
    using ArrangePolygon = arrangement::ArrangePolygon;

    ArrangePolygon ap = obj.get_arrange_polygon();
    ap.bed_idx        = ap.translation.x() / bed_stride(plater);
    ap.setter         = [obj, plater](const ArrangePolygon &p) {
        if (p.is_arranged()) {
            Vec2d t = p.translation.cast<double>();
            t.x() += p.bed_idx * bed_stride(plater);
            T{obj}.apply_arrange_result(t, p.rotation);
        }
    };

    return ap;
}

template<>
arrangement::ArrangePolygon get_arrange_poly(ModelInstance *inst,
                                             const Plater * plater);

arrangement::ArrangeParams get_arrange_params(Plater *p);

}} // namespace Slic3r::GUI

#endif // ARRANGEJOB_HPP
