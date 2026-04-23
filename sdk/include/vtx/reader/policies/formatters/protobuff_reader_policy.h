#pragma once
#include <stop_token>
#include <string>
#include <vector>
#include "vtx/common/vtx_types.h"


namespace cppvtx {
    class FileHeader;
    class FileFooter;
    class ContextualSchema;
} // namespace cppvtx
namespace VTX {

    struct ProtobufReaderPolicy {
        using HeaderType = cppvtx::FileHeader;
        using FooterType = cppvtx::FileFooter;
        using SchemaType = cppvtx::ContextualSchema;

        static std::string GetMagicBytes();
        static HeaderType ParseHeader(const std::string& buffer);
        static FooterType ParseFooter(const std::string& buffer);
        static void ProcessChunkData(int chunk_index, const std::string& compressed, std::stop_token st,
                                     std::vector<VTX::Frame>& out_native_frames,
                                     std::vector<uint8_t>& out_decompressed_blob,
                                     std::vector<std::span<const std::byte>>& out_raw_frames_spans);
        static void PopulateIndexTable(const FooterType& footer, std::vector<ChunkIndexEntry>& chunk_index_table);
        static void PopulateGameTimes(const FooterType& footer, VTX::GameTime::VTXGameTimes& game_times);
        static int32_t GetTotalFrames(const FooterType& footer);
        static const SchemaType& GetSchema(const HeaderType& header);
        static VTX::ContextualSchema GetVTXContextualSchema(const HeaderType& header);
        static VTX::FileHeader GetVTXHeader(const HeaderType& fbs_header);
        static VTX::FileFooter GetVTXFooter(const FooterType& fbs_footer);
    };
} // namespace VTX
