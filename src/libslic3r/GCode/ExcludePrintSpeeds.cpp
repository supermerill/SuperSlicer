#include "ExcludePrintSpeeds.hpp"

#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <regex>

namespace Slic3r {

ExcludePrintSpeeds::ExcludePrintSpeeds(const std::string &_forbidden_ranges_user_input,
    const ConfigOptionEnum<ExcludePrintSpeedsAdjustmentDirection> &_adjustment_direction)
{
    adjustment_direction = _adjustment_direction;

    std::vector<std::string> excluded_ranges_strings = split_user_input_ranges_to_individual_strings(_forbidden_ranges_user_input);
    parse_input(excluded_ranges_strings);
    check_input_correctness(excluded_ranges_strings);
}

std::vector<std::string> ExcludePrintSpeeds::split_user_input_ranges_to_individual_strings(const std::string &_excluded_ranges_user_input)
{
    std::string forbidden_ranges_user_input = _excluded_ranges_user_input;
    forbidden_ranges_user_input.erase(std::remove_if(forbidden_ranges_user_input.begin(),
                                                     forbidden_ranges_user_input.end(), ::isspace),
                                      forbidden_ranges_user_input.end());

    std::vector<std::string> excluded_ranges_strings;
    boost::split(excluded_ranges_strings, forbidden_ranges_user_input, boost::is_any_of(","));
    return excluded_ranges_strings;
}


void ExcludePrintSpeeds::parse_input(std::vector<std::string> excluded_ranges_strings)
{
    auto numeric_range_regex = std::regex("^(\\d+)-(\\d+)$");
    for (const auto &elem : excluded_ranges_strings) {
        std::smatch regex_match;
        if (!std::regex_match(elem, regex_match, numeric_range_regex)) {
            throw Slic3r::SlicingError("Invalid range " + elem +
                                       ". Range must have start and end values,"
                                       " separated by \"-\" (example: 30 - 50)");
        }

        auto lower_bound  = std::stoi(regex_match[LOWER_BOUND_MATCH_INDEX]);
        auto higher_bound = std::stoi(regex_match[UPPER_BOUND_MATCH_INDEX]);
        excluded_ranges.emplace_back(lower_bound, higher_bound);
    }
}

// TODO - CHKA. The strings here been whitespace-filtered. I want to print the original
//  user input for HCI reasons. Not strict, might skip doing this.
void ExcludePrintSpeeds::check_input_correctness(const std::vector<std::string> &excluded_ranges_strings)
{
    size_t i = 0;
    for (const auto &range : excluded_ranges) {
        if (range.first >= range.second) {
            throw Slic3r::SlicingError("Invalid range " + excluded_ranges_strings[i] +
                                       ". Upper bound must be greater than lower bound.");
        }
        i++;
    }

    // Check range consistency (Simply check for overlap. User can enter them in non-ascending lower bound order)
    std::sort(excluded_ranges.begin(), excluded_ranges.end(),
              [](auto &left, auto &right) { return left.first < right.first; });

    for (i = 1; i < excluded_ranges.size(); i++) {
        if (excluded_ranges[i].first < excluded_ranges[i - 1].second) {
            throw Slic3r::SlicingError("Ranges " + excluded_ranges_strings[i - 1] + " and " +
                                       excluded_ranges_strings[i] + " overlap.");
        }
    }
}


double_t ExcludePrintSpeeds::adjust_speed_if_in_forbidden_range(double speed)
{
    for (auto range : excluded_ranges) {
        if (speed > range.first && speed < range.second) {
            switch (adjustment_direction) {
            case epsdLowest:
                speed = range.first;
                break;
            case epsdHighest:
                speed = range.second;
                break;
            case epsdNearest:
                if ((speed - range.first) < (range.second - speed)) {
                    speed = range.first;
                } else {
                    speed = range.second;
                }
                break;
            default:
                return speed;
            }

            return speed;
        }
    }

    return speed;
}

}