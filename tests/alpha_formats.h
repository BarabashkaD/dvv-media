#pragma once

// Two custom formats with an alpha component, shared by the host and the GPU display tests: an
// 8-bit gray + alpha and a planar 10-bit Y U V A (4:4:4, little-endian words).

#include <string_view>

#include "dvv/base/error.h"
#include "dvv/base/json.h"
#include "dvv/media/format_json.h"
#include "dvv/media/types.h"

namespace dvv::media::test {

inline constexpr std::string_view YA8_JSON = R"({
  "name": "test-ya8",
  "model": "gray",
  "widthMultiple": 1,
  "components": [
    {"kind": "Y", "bits": 8, "log2SubX": 0, "log2SubY": 0},
    {"kind": "A", "bits": 8, "log2SubX": 0, "log2SubY": 0}
  ],
  "planes": [
    {"groupPixels": 1, "groupBytes": 2, "wordBytes": 1, "byteOrder": "little", "rowAlign": 1,
     "log2SubY": 0, "slots": [{"component": 0, "index": 0, "word": 0, "shift": 0},
                              {"component": 1, "index": 0, "word": 1, "shift": 0}]}
  ]
})";

inline constexpr std::string_view YUVA10_JSON = R"({
  "name": "test-yuva444p10",
  "model": "yuv",
  "widthMultiple": 1,
  "components": [
    {"kind": "Y", "bits": 10, "log2SubX": 0, "log2SubY": 0},
    {"kind": "U", "bits": 10, "log2SubX": 0, "log2SubY": 0},
    {"kind": "V", "bits": 10, "log2SubX": 0, "log2SubY": 0},
    {"kind": "A", "bits": 10, "log2SubX": 0, "log2SubY": 0}
  ],
  "planes": [
    {"groupPixels": 1, "groupBytes": 2, "wordBytes": 2, "byteOrder": "little", "rowAlign": 1,
     "log2SubY": 0, "slots": [{"component": 0, "index": 0, "word": 0, "shift": 0}]},
    {"groupPixels": 1, "groupBytes": 2, "wordBytes": 2, "byteOrder": "little", "rowAlign": 1,
     "log2SubY": 0, "slots": [{"component": 1, "index": 0, "word": 0, "shift": 0}]},
    {"groupPixels": 1, "groupBytes": 2, "wordBytes": 2, "byteOrder": "little", "rowAlign": 1,
     "log2SubY": 0, "slots": [{"component": 2, "index": 0, "word": 0, "shift": 0}]},
    {"groupPixels": 1, "groupBytes": 2, "wordBytes": 2, "byteOrder": "little", "rowAlign": 1,
     "log2SubY": 0, "slots": [{"component": 3, "index": 0, "word": 0, "shift": 0}]}
  ]
})";

// Loads one of the formats above (the same descriptor interns to the same id on every call).
[[nodiscard]] inline Result<FormatId> loadFormat(std::string_view text) {
    DVV_TRY(const json::Value value, json::parse(text));
    return formatFromJson(value);
}

}  // namespace dvv::media::test
