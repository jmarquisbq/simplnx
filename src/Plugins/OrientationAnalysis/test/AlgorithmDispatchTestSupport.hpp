#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/Common/Types.hpp"

namespace nx::core::UnitTest
{
enum class AlgorithmDispatchPath : uint8
{
  Unknown,
  Direct,
  Scanline
};

ORIENTATIONANALYSIS_EXPORT AlgorithmDispatchPath GetAlgorithmDispatchPathFromOrientationAnalysisPlugin();
} // namespace nx::core::UnitTest
