#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "dvv/media/display_vulkan.h"
#include "dvv/media/frame_vulkan.h"
#include "dvv/media/rate_size_text.h"
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
    // The headers new in 0.2.0 are installed: one symbol from each.
    const auto size = parseSize("1920x1080");
    if (!size || *size != std::pair<std::uint32_t, std::uint32_t>{1920, 1080}) {
        std::puts("parseSize(1920x1080) differs");
        ok = false;
    }
    if (vulkan::BUFFER == vulkan::IMAGE) {
        std::puts("vulkan::BUFFER equals vulkan::IMAGE");
        ok = false;
    }
    if (DISPLAY_IMAGE_FORMAT != VK_FORMAT_R16G16B16A16_SFLOAT) {
        std::puts("DISPLAY_IMAGE_FORMAT differs");
        ok = false;
    }
    // dvv::media_sources is linked as a component library: its providers are registered.
    const auto registry = dvv::ComponentRegistry::create(dvv::Capabilities{});
    const auto sources = registry->list<dvv::media::ISourceProvider>();
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
