#pragma once

#include <string>

#include "infrastructure/process.h"

namespace jkm {

std::string BuildDownloadHelperScript();
ProcessOutputLineHandler BuildDownloadOutputLineHandler();

}  // namespace jkm
