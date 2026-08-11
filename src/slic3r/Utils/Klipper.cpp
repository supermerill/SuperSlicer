///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "Klipper.hpp"

#include <algorithm>
#include <exception>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <wx/progdlg.h>
#include <wx/string.h>

#include "libslic3r/Updater/Http.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {

Klipper::Klipper(DynamicPrintConfig *config) : OctoPrint(config) {}

const char* Klipper::get_name() const { return "Klipper"; }

}
