#pragma once

#include "Anchoring.hpp"

#include <string>
#include <vector>

namespace reflow {

std::string encodeAnchors(const std::vector<Anchor> &anchors);
bool decodeAnchors(const std::string &encoded, std::vector<Anchor> &anchors);

std::string encodePlan(const Plan &plan);
bool decodePlan(const std::string &encoded, Plan &plan);

}
