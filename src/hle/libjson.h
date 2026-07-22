#pragma once
// sce::Json::Value / sce::Json::String HLE handlers.
//
// Ported from SharpEmu's JsonValueExports.cs + JsonExports.cs model.
// The Construct runtime (Dreaming Sarah) uses sce::Json::Value::set(double)
// and Value(double) constructor to create float variants during data.js
// JSON parsing.  Intercepting these via HLE ensures the double value is
// correctly stored in the guest variant, bypassing the runtime's buggy
// internal number parser.
//
// sce::Json::Value layout (guessed from Prospero/Orbis ABI):
//   [0..3]  type tag (u32): 0=Null, 1=Boolean, 2=Integer, 3=Unsigned,
//                           4=Real, 5=String, 6=Array, 7=Object
//   [4..7]  padding/flags (u32)
//   [8..15] data (union of i64/u64/double/pointer)
//   [16..23] string length / container size (u64)
// Total: 24 bytes
//
// sce::Json::String layout:
//   [0..7]  pointer to character data (or inline chars if short)
//   [8..15] length (u64)
// Total: 16 bytes

namespace HLE {
    void RegisterLibJson();
}
