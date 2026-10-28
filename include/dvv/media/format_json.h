#pragma once

// Custom pixel formats as JSON (spec §7.3, M2 design D3). The schema is in the dvv-media README.

#include "dvv/base/error.h"
#include "dvv/base/json.h"
#include "dvv/media/types.h"

namespace dvv::media {

// Every key written explicitly, defaults included. An unknown id gives a JSON null.
[[nodiscard]] json::Value formatToJson(FormatId format);
// Parses strictly, validates (validate(), incl. the GPU limits) and interns. Idempotent: an equal
// descriptor already interned under that name returns its id. Errc::Format for a malformed value,
// an invalid descriptor, a catalogue name, a name ending in "@planes", or a different descriptor
// already interned under that name.
[[nodiscard]] Result<FormatId> formatFromJson(const json::Value& value);

}  // namespace dvv::media
