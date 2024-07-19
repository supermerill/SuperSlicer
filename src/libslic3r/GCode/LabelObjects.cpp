#include "LabelObjects.hpp"

#include "ClipperUtils.hpp"
#include "Geometry/ConvexHull.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "TriangleMeshSlicer.hpp"

#include <boost/regex.hpp>

namespace Slic3r::GCode {


namespace {

Polygon instance_outline(const PrintInstance* pi)
{
    ExPolygons outline;
    const ModelObject* mo = pi->model_instance->get_object();
    const ModelInstance* mi = pi->model_instance;
    for (const ModelVolume *v : mo->volumes) {
        Polygons vol_outline;
        vol_outline = project_mesh(v->mesh().its,
                                    mi->get_matrix() * v->get_matrix(),
                                    [] {});
        switch (v->type()) {
        case ModelVolumeType::MODEL_PART: outline = union_ex(outline, vol_outline); break;
        case ModelVolumeType::NEGATIVE_VOLUME: outline = diff_ex(outline, vol_outline); break;
        default:;
        }
    }

    // The projection may contain multiple polygons, which is not supported by Klipper.
    // When that happens, calculate and use a 2d convex hull instead.
    if (outline.size() == 1u)
        return outline.front().contour;
    else
        return pi->model_instance->get_object()->convex_hull_2d(pi->model_instance->get_matrix());
}

}; // anonymous namespace


void LabelObjects::init(const Print& print)
{
    m_label_objects_style = print.config().gcode_label_objects;
    m_flavor = print.config().gcode_flavor;

    if (m_label_objects_style == LabelObjectsStyle::Disabled)
        return;

    std::map<const ModelObject*, std::vector<const PrintInstance*>> model_object_to_print_instances;

    // Iterate over all PrintObjects and their PrintInstances, collect PrintInstances which
    // belong to the same ModelObject.
    for (const PrintObject* po : print.objects())
        for (const PrintInstance& pi : po->instances())
            model_object_to_print_instances[pi.model_instance->get_object()].emplace_back(&pi);
    
    // Now go through the map, assign a unique_id to each of the PrintInstances and get the indices of the
    // respective ModelObject and ModelInstance so we can use them in the tags. This will maintain
    // indices even in case that some instances are rotated (those end up in different PrintObjects)
    // or when some are out of bed (these ModelInstances have no corresponding PrintInstances).
    std::regex pattern("[^\\w]+", std::regex_constants::ECMAScript);
    int unique_id = 0;
    for (const auto& [model_object, print_instances] : model_object_to_print_instances) {
        const ModelObjectPtrs& model_objects = model_object->get_model()->objects;
        int object_id = int(std::find(model_objects.begin(), model_objects.end(), model_object) - model_objects.begin());
        bool object_has_more_instances = print_instances.size() > 1u;
        for (const PrintInstance* const pi : print_instances) {
            int instance_id = int(std::find(model_object->instances.begin(), model_object->instances.end(), pi->model_instance) - model_object->instances.begin());

            // Now compose the name of the object and define whether indexing is 0 or 1-based.
            // name only composed of alphanumeric & '_'.
            const std::string obj_name = std::regex_replace(model_object->name, pattern, std::string("_"));
            std::string name = obj_name;
            if (m_label_objects_style == LabelObjectsStyle::Firmware) {
                // use one-based indexing for objects and instances so indices match what we see in PrusaSlicer.
                if (object_has_more_instances)
                    name += " (Instance " + std::to_string(instance_id+1) + ")";
            }
            else if (m_label_objects_style != LabelObjectsStyle::Disabled) {
                // use zero-based indexing for objects and instances, as we always have done
                name += " id:" + std::to_string(object_id) + " copy " + std::to_string(instance_id); 
            }
            if (m_flavor == gcfKlipper) {
                const std::string banned = "\b\t\n\v\f\r \"#%&\'*-./:;<>\\";
                std::replace_if(name.begin(), name.end(), [&banned](char c) { return banned.find(c) != std::string::npos; }, '_');
            }

            m_label_data.emplace(pi, LabelData{name, unique_id, obj_name, object_id, instance_id});
            ++unique_id;
        }
    }
}



std::string LabelObjects::all_objects_header(BoundingBoxf3 &global_bounding_box, coordf_t resolution) const
{
    if (m_label_objects_style == LabelObjectsStyle::Disabled)
        return std::string();

    std::string out;
    Polygon global_outline;

    // Let's sort the values according to unique_id so they are in the same order in which they were added.
    std::vector<std::pair<const PrintInstance*, LabelData>> label_data_sorted;
    for (const auto& pi_and_label : m_label_data)
        label_data_sorted.emplace_back(pi_and_label);
    std::sort(label_data_sorted.begin(), label_data_sorted.end(), [](const auto& ld1, const auto& ld2) { return ld1.second.unique_id < ld2.second.unique_id; });
    
    char buffer[64];
    out += "\n";
    for (const auto& [print_instance, label] : label_data_sorted) {
        // create bounding box
        BoundingBoxf3 bounding_box = print_instance->model_instance->get_object()->instance_bounding_box(*print_instance->model_instance, false);
        if (global_bounding_box.size().norm() == 0) {
            global_bounding_box = bounding_box;
        } else {
            global_bounding_box.merge(bounding_box);
        }
        // create outline (polygon)
        Polygon outline = instance_outline(print_instance);
        assert(! outline.empty());
        outline.douglas_peucker(resolution); //0.1f (prusa: static 0.05f)
        Point center = outline.centroid();
        //update global outline
        global_outline = Geometry::convex_hull(Polygons{outline, global_outline});
        //use data for printing firmware-specific stuff.
        if ((m_label_objects_style == LabelObjectsStyle::Firmware || m_label_objects_style == LabelObjectsStyle::Both)
                && m_flavor == gcfKlipper)  {
            //start EXCLUDE_OBJECT_DEFINE line with name
            out.append("EXCLUDE_OBJECT_DEFINE NAME='").append(label.unique_name).append("'");
            // add centroid of object.
            std::snprintf(buffer, sizeof(buffer) - 1, " CENTER=%.3f,%.3f", unscale<float>(center[0]), unscale<float>(center[1]));
            out.append(buffer).append(" POLYGON=[");
            for (const Point& point : outline) {
                std::snprintf(buffer, sizeof(buffer) - 1, "[%.3f,%.3f],", unscale<float>(point[0]), unscale<float>(point[1]));
                out += buffer;
            }
            out.pop_back(); //remove last ','
            out += "]\n";
        } else {
            if (m_flavor == gcfMarlinFirmware /*prusaFirmware*/) {
                // really needed by someone? let it for prusa firmware.
                out += start_object(*print_instance, IncludeName::Yes);
                out += stop_object(*print_instance);
            }
        }
        // add object json
        out.append("; object:{\"name\":\"").append(label.unique_name).append("\"")
            .append(",\"id\":\"").append(std::to_string(label.unique_id)).append("\"")
            .append(",\"object_id\":").append(std::to_string(label.unique_id))
            .append(",\"copy\":").append(std::to_string(label.copy_id));
        std::snprintf(buffer, sizeof(buffer) - 1, "%.3f,%.3f,%.3f", unscale<float>(center[0]), unscale<float>(center[1]), 0.f);
        out.append(",\"object_center\":[").append(buffer).append("]");
        std::snprintf(buffer, sizeof(buffer) - 1, "%.3f,%.3f,%.3f",bounding_box.center().x(), bounding_box.center().y(), bounding_box.center().z());
        out.append(",\"boundingbox_center\":[").append(buffer).append("]");
        std::snprintf(buffer, sizeof(buffer) - 1, "%.3f,%.3f,%.3f", bounding_box.size().x(), bounding_box.size().y(), bounding_box.size().z());
        out.append(",\"boundingbox_size\":[").append(buffer).append("]");
        out.append(",\"outline\":[");
        for (const Point& point : outline) {
            std::snprintf(buffer, sizeof(buffer) - 1, "[%.3f,%.3f],", unscale<float>(point[0]), unscale<float>(point[1]));
            out += buffer;
        }
        out.pop_back(); //remove last ','
        out += "]}\n";
    }
    if ( (m_label_objects_style == LabelObjectsStyle::Firmware || m_label_objects_style == LabelObjectsStyle::Both)  
        && (m_flavor == gcfMarlinLegacy || m_flavor == gcfMarlinFirmware || m_flavor == gcfRepRap)) {
        out.append("; Total objects to print: ").append(std::to_string(m_label_data.size())).append("\n")
            .append("M486 T").append(std::to_string(m_label_data.size())).append("\n");
    }
    // add plater json
    {
        out.append("; plater:{");
        Point global_center = global_outline.centroid();
        std::snprintf(buffer, sizeof(buffer) - 1, "%.3f,%.3f,%.3f", unscaled(global_center.x()), unscaled(global_center.y()), 0.f);
        out.append(",\"object_center\":[").append(buffer).append("]");
        std::snprintf(buffer, sizeof(buffer) - 1, "%.3f,%.3f,%.3f", global_bounding_box.center().x(), global_bounding_box.center().y(),
                      global_bounding_box.center().z());
        out.append(",\"boundingbox_center\":[").append(buffer).append("]");
        std::snprintf(buffer, sizeof(buffer) - 1, "%.3f,%.3f,%.3f", global_bounding_box.size().x(), global_bounding_box.size().y(),
                      global_bounding_box.size().z());
        out.append(",\"boundingbox_size\":[").append(buffer).append("]");
        out.append(",\"outline\":[");
        for (const Point &point : global_outline) {
            std::snprintf(buffer, sizeof(buffer) - 1, "[%.3f,%.3f],", unscale<float>(point[0]), unscale<float>(point[1]));
            out += buffer;
        }
        out.pop_back(); // remove last ','
        out += "]}\n\n";
    }
    return out;
}



std::string LabelObjects::start_object(const PrintInstance& print_instance, IncludeName include_name) const
{
    if (m_label_objects_style == LabelObjectsStyle::Disabled)
        return std::string();

    const LabelData& label = m_label_data.at(&print_instance);

    std::string out;
    if (m_label_objects_style == LabelObjectsStyle::Octoprint || m_label_objects_style == LabelObjectsStyle::Both)
        out += std::string("; printing object ") + label.unique_name + "\n";
    if (m_label_objects_style == LabelObjectsStyle::Firmware || m_label_objects_style == LabelObjectsStyle::Both) {
        if (m_flavor == GCodeFlavor::gcfMarlinFirmware || m_flavor == GCodeFlavor::gcfMarlinLegacy || m_flavor == GCodeFlavor::gcfRepRap) {
            out += std::string("M486 S") + std::to_string(label.unique_id);
            if (include_name == IncludeName::Yes) {
                out += (m_flavor == GCodeFlavor::gcfRepRap ? " A" : "\nM486 A");
                out += (m_flavor == GCodeFlavor::gcfRepRap ? (std::string("\"") + label.unique_name + "\"") : label.unique_name);
            }
            out += "\n";
        } else if (m_flavor == gcfKlipper)
            out += "EXCLUDE_OBJECT_START NAME='" + label.unique_name + "'\n";
        else {
            // Not supported by / implemented for the other firmware flavors.
        }
    }
    return out;
}



std::string LabelObjects::stop_object(const PrintInstance& print_instance) const
{
    if (m_label_objects_style == LabelObjectsStyle::Disabled)
        return std::string();

    const LabelData& label = m_label_data.at(&print_instance);

    std::string out;
    if (m_label_objects_style == LabelObjectsStyle::Octoprint || m_label_objects_style == LabelObjectsStyle::Both)
        out += std::string("; stop printing object ") + label.unique_name + "\n";
    if (m_label_objects_style == LabelObjectsStyle::Firmware || m_label_objects_style == LabelObjectsStyle::Both) {
        if (m_flavor == GCodeFlavor::gcfMarlinFirmware || m_flavor == GCodeFlavor::gcfMarlinLegacy || m_flavor == GCodeFlavor::gcfRepRap)
            out += std::string("M486 S-1\n");
        else if (m_flavor ==gcfKlipper)
            out += "EXCLUDE_OBJECT_END NAME='" + label.unique_name + "'\n";
        else {
            // Not supported by / implemented for the other firmware flavors.
        }
    }
    return out;
}



} // namespace Slic3r::GCode
