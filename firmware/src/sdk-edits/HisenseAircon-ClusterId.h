#pragma once
#include <lib/core/DataModelTypes.h>
namespace chip {
namespace app {
namespace Clusters {
namespace HisenseAircon {
inline constexpr ClusterId Id = 0xFFF1FC00;

// Attribute ids for the ember-only Hisense manufacturer cluster. No .zap-generated
// accessors exist for a custom cluster, so these back the raw case labels /
// emberAfWriteAttribute calls in matter_drivers.cpp (kept here so the literals
// live in one place, mirroring the CHIP Clusters::X::Attributes::Y::Id style).
namespace Attributes {
namespace Eco          { inline constexpr AttributeId Id = 0x0000; }
namespace Turbo        { inline constexpr AttributeId Id = 0x0001; }
namespace Mute         { inline constexpr AttributeId Id = 0x0002; }
namespace SleepProfile { inline constexpr AttributeId Id = 0x0003; }
namespace CompressorHz { inline constexpr AttributeId Id = 0x0010; }
namespace OutdoorTemp  { inline constexpr AttributeId Id = 0x0011; }
namespace Features1    { inline constexpr AttributeId Id = 0x0012; }  // packed HisenseFeatures (docs/14)
namespace Faults1      { inline constexpr AttributeId Id = 0x0013; }  // packed HisenseFaults   (docs/14)
} // namespace Attributes

} // namespace HisenseAircon
} // namespace Clusters
} // namespace app
} // namespace chip
