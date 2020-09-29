/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "util/optional.h"

namespace pbrt {

struct ParsedURI {
    std::string protocol{};
    std::string username{};
    std::string password{};
    std::string host{};
    Optional<uint16_t> port{};
    std::string path{};
    std::unordered_map<std::string, std::string> options{};

    ParsedURI(const std::string& uri);
};

}  // namespace pbrt
