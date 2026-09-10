#include "vtx/common/vtx_replay_framing.h"

#include <string>

#include <zstd.h>

namespace VTX {
    namespace Framing {

        // The one zstd call in the framing; kept out of the header so consumers that
        // only include reader/common headers need no zstd headers on their path.
        std::string Compress(std::string payload, int8_t level) {
            const size_t max_size = ZSTD_compressBound(payload.size());
            std::string compressed(max_size, '\0');
            const size_t compressed_size =
                ZSTD_compress(compressed.data(), max_size, payload.data(), payload.size(), level);
            if (ZSTD_isError(compressed_size) || compressed_size >= payload.size()) {
                return payload;
            }
            compressed.resize(compressed_size);
            return compressed;
        }

    } // namespace Framing
} // namespace VTX
