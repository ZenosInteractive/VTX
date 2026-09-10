#pragma once
// Read-back helpers shared by the replay rewrite tests (framing, cut, filter):
// open a .vtx, pull every frame, and check a footer's seek table against the
// bytes actually on disk through the shared framing definitions.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "vtx/common/vtx_replay_framing.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"

namespace VtxTest {

    struct ReplaySnapshot {
        bool ok = false;
        VTX::VtxFormat format = VTX::VtxFormat::Unknown;
        VTX::FileHeader header;
        VTX::ContextualSchema schema;
        VTX::FileFooter footer;
        std::vector<VTX::Frame> frames;
    };

    /// Opens @p path and copies out header, schema, footer and every frame.
    inline ReplaySnapshot ReadReplay(const std::string& path) {
        ReplaySnapshot s;
        VTX::ReaderContext ctx = VTX::OpenReplayFile(path);
        if (!ctx.Loaded()) {
            ADD_FAILURE() << "open failed for " << path << ": " << ctx.GetError().message;
            return s;
        }
        if (!ctx.WaitUntilReady()) {
            ADD_FAILURE() << "load failed for " << path << ": " << ctx.GetReadyError().message;
            return s;
        }
        s.format = ctx.format;
        s.header = ctx->GetHeader();
        s.schema = ctx->GetContextualSchema();
        s.footer = ctx->GetFooter();
        const int32_t total = ctx->GetTotalFrames();
        s.frames.reserve(static_cast<size_t>(total));
        for (int32_t i = 0; i < total; ++i) {
            const VTX::Frame* frame = ctx->GetFrameSync(i);
            if (!frame) {
                ADD_FAILURE() << "frame " << i << " unreadable in " << path;
                return s;
            }
            s.frames.push_back(*frame);
        }
        s.ok = true;
        return s;
    }

    /// Field-by-field equality of two entities (scalars, math types, the common arrays, container counts).
    inline void ExpectEntityEqual(const VTX::PropertyContainer& a, const VTX::PropertyContainer& b,
                                  const std::string& what) {
        EXPECT_EQ(a.entity_type_id, b.entity_type_id) << what;
        EXPECT_EQ(a.content_hash, b.content_hash) << what;
        EXPECT_EQ(a.bool_properties, b.bool_properties) << what;
        EXPECT_EQ(a.int32_properties, b.int32_properties) << what;
        EXPECT_EQ(a.int64_properties, b.int64_properties) << what;
        EXPECT_EQ(a.float_properties, b.float_properties) << what;
        EXPECT_EQ(a.double_properties, b.double_properties) << what;
        EXPECT_EQ(a.string_properties, b.string_properties) << what;
        EXPECT_EQ(a.transform_properties, b.transform_properties) << what;
        EXPECT_EQ(a.vector_properties, b.vector_properties) << what;
        EXPECT_EQ(a.quat_properties, b.quat_properties) << what;
        EXPECT_EQ(a.range_properties, b.range_properties) << what;
        EXPECT_EQ(a.int32_arrays.data, b.int32_arrays.data) << what;
        EXPECT_EQ(a.int32_arrays.offsets, b.int32_arrays.offsets) << what;
        EXPECT_EQ(a.float_arrays.data, b.float_arrays.data) << what;
        EXPECT_EQ(a.float_arrays.offsets, b.float_arrays.offsets) << what;
        EXPECT_EQ(a.string_arrays.data, b.string_arrays.data) << what;
        EXPECT_EQ(a.string_arrays.offsets, b.string_arrays.offsets) << what;
        EXPECT_EQ(a.vector_arrays.data, b.vector_arrays.data) << what;
        EXPECT_EQ(a.any_struct_properties.size(), b.any_struct_properties.size()) << what;
        EXPECT_EQ(a.map_properties.size(), b.map_properties.size()) << what;
    }

    /// Every seek-table entry must describe the bytes on disk: the size prefix equals
    /// chunk_size_bytes - prefix, the xxHash64 of the payload matches, and chunks are
    /// contiguous from the first one onwards.
    inline void ExpectSeekTableMatchesFile(const std::string& path, const VTX::FileFooter& footer) {
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in) << path;
        const uint64_t file_size = VTX::Framing::StreamSize(in);
        ASSERT_FALSE(footer.chunk_index.empty());
        for (const VTX::ChunkIndexEntry& entry : footer.chunk_index) {
            ASSERT_LE(entry.file_offset + entry.chunk_size_bytes, file_size) << "chunk " << entry.chunk_index;
            char prefix[VTX::Framing::kSizePrefixSize];
            ASSERT_TRUE(VTX::Framing::ReadAt(in, entry.file_offset, prefix, sizeof(prefix)));
            const uint32_t payload_size = VTX::Framing::DecodeSizePrefix(prefix);
            ASSERT_EQ(VTX::Framing::ChunkSizeOnDisk(payload_size), entry.chunk_size_bytes)
                << "chunk " << entry.chunk_index;
            std::string payload(payload_size, '\0');
            ASSERT_TRUE(in.read(payload.data(), payload_size));
            EXPECT_EQ(VTX::Framing::PayloadChecksum(payload), entry.checksum) << "chunk " << entry.chunk_index;
        }
        for (size_t i = 1; i < footer.chunk_index.size(); ++i) {
            EXPECT_EQ(footer.chunk_index[i].file_offset,
                      footer.chunk_index[i - 1].file_offset + footer.chunk_index[i - 1].chunk_size_bytes);
        }
    }

} // namespace VtxTest
