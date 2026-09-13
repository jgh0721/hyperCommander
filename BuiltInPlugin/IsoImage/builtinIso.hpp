#pragma once
#include "pluginHost/approvedWcxBackend.hpp"

namespace hc::plugins {
// Pure construction. Approval comes from a build-generated compiled manifest;
// the usual inspector verifies on-disk binaries on every host connection.
ApprovedWcxOptions BuiltinIsoOptions();
}
