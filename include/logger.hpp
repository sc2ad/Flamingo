#pragma once

#include "paper/shared/logger.hpp"

namespace flamingo {

// Register file log in main.cpp
static constexpr auto Logger = Paper::ConstLoggerContext("Flamingo"); //Paper::Logger::WithContext<"Flamingo", false>();

}