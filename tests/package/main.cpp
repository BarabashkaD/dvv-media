#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "dvv/media/source_factory.h"
#include "dvv/media/types.h"
#include "dvv/media/uri_colour.h"
#include "dvv/registry/capabilities.h"
#include "dvv/registry/component_info.h"
#include "dvv/registry/component_registry.h"

int main() {
    using namespace dvv::media;
    const ColourParams wanted{.matrix = ColourMatrix::Bt709, .range = ColourRange::Limited};
    const ColourId id = internColour(wanted);
    const ColourParams& got = colourParams(id);
    bool ok = true;
    if (got != wanted) {
        std::printf(
            "colourParams(%u) differs: matrix %d/%d, range %d/%d, transfer %d/%d, "
            "primaries %d/%d (got/wanted)\n",
            static_cast<unsigned>(id), static_cast<int>(got.matrix),
            static_cast<int>(wanted.matrix), static_cast<int>(got.range),
            static_cast<int>(wanted.range), static_cast<int>(got.transfer),
            static_cast<int>(wanted.transfer), static_cast<int>(got.primaries),
            static_cast<int>(wanted.primaries));
        ok = false;
    }
    const ColourId unspecified = internColour(ColourParams{});
    if (unspecified != 0) {
        std::printf("internColour(ColourParams{}) is %u, not 0\n",
                    static_cast<unsigned>(unspecified));
        ok = false;
    }
    // The colour query moved into the core library (dvv/media/uri_colour.h).
    const auto parameters = colourParameters(ColourParams{.range = ColourRange::Full});
    if (parameters != std::vector<std::pair<std::string, std::string>>{{"range", "full"}}) {
        std::printf("colourParameters(range=full) differs: %zu entries:", parameters.size());
        for (const auto& [name, value] : parameters) {
            std::printf(" %s=%s", name.c_str(), value.c_str());
        }
        std::puts("");
        ok = false;
    }
    // dvv::media_sources is linked as a component library: its providers are registered.
    const auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{});
    const auto sources = registry->list(dvv::Kind::Source);
    if (std::ranges::none_of(sources, [](const dvv::ComponentStatus& status) {
            return status.info.id == "synthetic";
        })) {
        std::puts("the source provider 'synthetic' is not registered");
        ok = false;
    }
    if (!ok) {
        return 1;
    }
    std::puts("dvv_media package works");
    return 0;
}
