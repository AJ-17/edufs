#include <string>

#include "doctest.h"
#include "edufs/version.hpp"

TEST_CASE("library links and reports a version") {
    CHECK(std::string(edufs::version()) == "0.3.0");
}