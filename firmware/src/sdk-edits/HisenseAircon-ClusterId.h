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
// RS-485 bus diagnostics (docs/14). The ids are the MATTER_HISENSE_ATTR_* values in
// matter_aircon_map.h, which the ESP32 glue uses; a host test holds the two in step.
namespace ChecksumErrors     { inline constexpr AttributeId Id = 0x0014; }  // int32u, since boot
namespace ReplyTimeouts      { inline constexpr AttributeId Id = 0x0015; }  // int32u, since boot
namespace UnansweredCommands { inline constexpr AttributeId Id = 0x0016; }  // int32u, since boot
namespace LinkLosses         { inline constexpr AttributeId Id = 0x0017; }  // int32u, since boot
namespace LinkToken          { inline constexpr AttributeId Id = 0x0018; }  // int16u, 0 = not learned yet
namespace BusLink            { inline constexpr AttributeId Id = 0x0019; }  // boolean
} // namespace Attributes

} // namespace HisenseAircon
} // namespace Clusters
} // namespace app
} // namespace chip
