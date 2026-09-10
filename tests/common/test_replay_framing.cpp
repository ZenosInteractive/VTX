// vtx_replay_framing.h is the one definition of the .vtx on-disk framing. These
// tests pin the byte layout of every helper and prove that a file written by the
// real sink is described exactly by ProbeLayout / PayloadChecksum -- which is what
// lets the sinks, repair, cut and filter share it without drifting apart.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include <xxh3.h>
#include <zstd.h> // the round-trip check decompresses directly

#include "vtx/common/vtx_replay_framing.h"
#include "vtx/common/vtx_types.h"
#include "vtx/writer/core/vtx_writer_facade.h"

#include "util/replay_snapshot.h"
#include "util/test_fixtures.h"

using namespace VTX::Framing;

namespace {

    std::string Repeated(size_t n, char c = 'a') {
        return std::string(n, c);
    }

    std::string RandomBytes(size_t n, uint32_t seed = 7) {
        std::mt19937 rng(seed);
        std::string out(n, '\0');
        for (char& c : out) {
            c = static_cast<char>(rng() & 0xFF);
        }
        return out;
    }

    // Host-order bytes of a u32, the way the sink has always written them.
    std::string HostBytes(uint32_t v) {
        std::string s(sizeof(v), '\0');
        std::memcpy(s.data(), &v, sizeof(v));
        return s;
    }

} // namespace

TEST(ReplayFraming, SizePrefixRoundTripsAndMatchesHostOrder) {
    for (uint32_t v : {0u, 1u, 512u, 0xDEADBEEFu, 0xFFFFFFFFu}) {
        const SizePrefix p = EncodeSizePrefix(v);
        EXPECT_EQ(std::string(p.data(), p.size()), HostBytes(v)) << v;
        EXPECT_EQ(DecodeSizePrefix(p.data()), v);
    }
    EXPECT_EQ(ChunkSizeOnDisk(0), kSizePrefixSize);
    EXPECT_EQ(ChunkSizeOnDisk(100), 104u);
}

TEST(ReplayFraming, BlocksHaveTheDocumentedLayout) {
    const std::string payload = "PAYLOAD";
    const std::string magic = "VTXF";

    const std::string chunk = ChunkBlock(payload);
    ASSERT_EQ(chunk.size(), kSizePrefixSize + payload.size());
    EXPECT_EQ(chunk.substr(0, 4), HostBytes(7));
    EXPECT_EQ(chunk.substr(4), payload);

    const std::string header = HeaderBlock(magic, payload);
    ASSERT_EQ(header.size(), 4 + 4 + payload.size());
    EXPECT_EQ(header.substr(0, 4), magic);
    EXPECT_EQ(header.substr(4, 4), HostBytes(7));
    EXPECT_EQ(header.substr(8), payload);

    const std::string trailer = FooterTrailer(7, magic);
    ASSERT_EQ(trailer.size(), kFooterTrailerSize);
    EXPECT_EQ(trailer.substr(0, 4), HostBytes(7));
    EXPECT_EQ(trailer.substr(4), magic);

    const std::string footer = FooterBlock(payload, magic);
    EXPECT_EQ(footer, payload + trailer);
}

TEST(ReplayFraming, CompressionRuleMatchesTheWriter) {
    EXPECT_FALSE(ShouldCompress(kCompressMinBytes - 1, true));
    EXPECT_TRUE(ShouldCompress(kCompressMinBytes, true));
    EXPECT_FALSE(ShouldCompress(1 << 20, false));

    // Compressible data shrinks and carries the zstd magic; the sniff agrees.
    const std::string big = Repeated(64 * 1024);
    const std::string compressed = Compress(big, kDefaultCompressionLevel);
    EXPECT_LT(compressed.size(), big.size());
    EXPECT_TRUE(LooksLikeZstd(compressed));
    EXPECT_FALSE(LooksLikeZstd(big));

    // Incompressible data is returned as-is (never grown).
    const std::string noise = RandomBytes(4096);
    EXPECT_EQ(Compress(noise, kDefaultCompressionLevel), noise);

    // The combined rule: threshold, enabled flag, then Compress.
    EXPECT_EQ(CompressIfBeneficial(Repeated(100)), Repeated(100)); // below threshold
    EXPECT_EQ(CompressIfBeneficial(big, /*enabled=*/false), big);
    EXPECT_EQ(CompressIfBeneficial(big), compressed);

    // Decompresses back to the input.
    std::string round(big.size(), '\0');
    const size_t n = ZSTD_decompress(round.data(), round.size(), compressed.data(), compressed.size());
    ASSERT_FALSE(ZSTD_isError(n));
    EXPECT_EQ(round, big);
}

TEST(ReplayFraming, ChecksumIsXxHash64OfThePayload) {
    const std::string a = RandomBytes(1000, 1);
    const std::string b = RandomBytes(1000, 2);
    EXPECT_EQ(PayloadChecksum(a), XXH3_64bits(a.data(), a.size()));
    EXPECT_EQ(PayloadChecksum(a.data(), a.size()), PayloadChecksum(a));
    EXPECT_NE(PayloadChecksum(a), PayloadChecksum(b));
}

TEST(ReplayFraming, ProbeLayoutLocatesHeaderAndFooterOfASyntheticFile) {
    const std::string magic = "VTXF";
    const std::string header = Repeated(40, 'h');
    const std::string chunk0 = Repeated(300, 'c');
    const std::string chunk1 = Repeated(20, 'd');
    const std::string footer = Repeated(64, 'f');

    std::stringstream file;
    file << HeaderBlock(magic, header) << ChunkBlock(chunk0) << ChunkBlock(chunk1) << FooterBlock(footer, magic);

    FileLayout layout;
    std::string error;
    ASSERT_TRUE(ProbeLayout(file, magic, layout, error)) << error;
    EXPECT_EQ(layout.file_size, file.str().size());
    EXPECT_EQ(layout.header_end, 4 + 4 + header.size());
    EXPECT_EQ(layout.footer_size, footer.size());
    EXPECT_EQ(layout.footer_offset, layout.file_size - kFooterTrailerSize - footer.size());
    EXPECT_FALSE(layout.footer_compressed);
    // The chunk region is exactly what sits between header and footer.
    EXPECT_EQ(layout.footer_offset - layout.header_end, ChunkBlock(chunk0).size() + ChunkBlock(chunk1).size());
    EXPECT_TRUE(IsZstdAt(file, layout.header_end + kSizePrefixSize, chunk0.size()) == false);

    // A compressed footer is detected through the sniff.
    const std::string zfooter = Compress(Repeated(4096, 'f'), kDefaultCompressionLevel);
    std::stringstream zfile;
    zfile << HeaderBlock(magic, header) << ChunkBlock(chunk0) << FooterBlock(zfooter, magic);
    ASSERT_TRUE(ProbeLayout(zfile, magic, layout, error)) << error;
    EXPECT_TRUE(layout.footer_compressed);
    EXPECT_EQ(layout.footer_size, zfooter.size());
}

TEST(ReplayFraming, ProbeRejectsBrokenLayouts) {
    const std::string magic = "VTXF";
    const std::string header = Repeated(40, 'h');
    FileLayout layout;
    std::string error;

    // Wrong magic.
    std::stringstream wrong;
    wrong << HeaderBlock("VTXP", header) << FooterBlock("f", "VTXP");
    EXPECT_FALSE(ProbeLayout(wrong, magic, layout, error));
    EXPECT_NE(error.find("magic"), std::string::npos) << error;

    // Footerless (crash-truncated): the header probes fine, the trailer does not.
    std::stringstream truncated;
    truncated << HeaderBlock(magic, header) << ChunkBlock(Repeated(300));
    uint64_t header_end = 0;
    ASSERT_TRUE(ProbeHeader(truncated, magic, StreamSize(truncated), header_end, error)) << error;
    EXPECT_EQ(header_end, 4 + 4 + header.size());
    EXPECT_FALSE(ProbeLayout(truncated, magic, layout, error));
    EXPECT_NE(error.find("no footer"), std::string::npos) << error;

    // A footer size that reaches back into the header.
    std::stringstream overlap;
    overlap << HeaderBlock(magic, header) << Repeated(10, 'x') << FooterTrailer(1000, magic);
    EXPECT_FALSE(ProbeLayout(overlap, magic, layout, error));
    EXPECT_FALSE(error.empty());

    // Empty / too small.
    std::stringstream empty;
    EXPECT_FALSE(ProbeLayout(empty, magic, layout, error));
}

TEST(ReplayFraming, CopyRangeStreamsExactBytes) {
    const std::string data = RandomBytes(10 * 1024 + 17);
    std::stringstream in(data);
    std::stringstream out;
    std::string error;
    ASSERT_TRUE(CopyRange(in, out, 100, 5000, error)) << error;
    EXPECT_EQ(out.str(), data.substr(100, 5000));

    std::stringstream none;
    ASSERT_TRUE(CopyRange(in, none, 0, 0, error)) << error;
    EXPECT_TRUE(none.str().empty());

    std::stringstream past;
    EXPECT_FALSE(CopyRange(in, past, data.size() - 10, 100, error));
    EXPECT_NE(error.find("Read failed"), std::string::npos) << error;
}

// ---------------------------------------------------------------------------
// The real writer and the framing helpers must agree byte for byte: probe a
// freshly recorded file and re-derive every seek-table field from the bytes.
// ---------------------------------------------------------------------------
class ReplayFramingOnDisk : public ::testing::TestWithParam<VTX::VtxFormat> {};

TEST_P(ReplayFramingOnDisk, SinkOutputIsDescribedByTheSharedFraming) {
    const bool flat = GetParam() == VTX::VtxFormat::FlatBuffers;
    const std::string magic = flat ? "VTXF" : "VTXP";

    VTX::WriterFacadeConfig cfg;
    cfg.output_filepath = VtxTest::OutputPath(std::string("framing_") + (flat ? "fb" : "pb") + ".vtx");
    cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
    cfg.replay_name = "FramingTest";
    cfg.replay_uuid = "uuid-framing";
    cfg.chunk_max_frames = 25;
    cfg.use_compression = true;
    auto writer = flat ? VTX::CreateFlatBuffersWriterFacade(cfg) : VTX::CreateProtobufWriterFacade(cfg);
    ASSERT_TRUE(writer);
    for (int i = 0; i < 60; ++i) {
        VTX::Frame frame;
        auto& bucket = frame.CreateBucket("entity");
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {"p", "n"};
        pc.int32_properties = {1, i, 0};
        pc.float_properties = {float(i), 1.0f};
        bucket.unique_ids.push_back("p");
        bucket.entities.push_back(std::move(pc));
        VTX::GameTime::GameTimeRegister t;
        t.game_time = float(i) / 60.0f;
        writer->RecordFrame(frame, t);
    }
    writer->Stop();

    const VtxTest::ReplaySnapshot s = VtxTest::ReadReplay(cfg.output_filepath);
    ASSERT_TRUE(s.ok);

    std::ifstream in(cfg.output_filepath, std::ios::binary);
    ASSERT_TRUE(in);
    FileLayout layout;
    std::string error;
    ASSERT_TRUE(ProbeLayout(in, magic, layout, error)) << error;

    // Chunks begin right after the header block and end right before the footer.
    ASSERT_FALSE(s.footer.chunk_index.empty());
    EXPECT_EQ(s.footer.chunk_index.front().file_offset, layout.header_end);
    const auto& last = s.footer.chunk_index.back();
    EXPECT_EQ(last.file_offset + last.chunk_size_bytes, layout.footer_offset);

    // Every entry's size prefix and checksum re-derive from the bytes on disk.
    VtxTest::ExpectSeekTableMatchesFile(cfg.output_filepath, s.footer);

    // Each chunk is either a zstd frame or raw, exactly as the sniff says.
    for (const auto& e : s.footer.chunk_index) {
        const bool z = IsZstdAt(in, e.file_offset + kSizePrefixSize, e.chunk_size_bytes - kSizePrefixSize);
        EXPECT_TRUE(z || e.chunk_size_bytes - kSizePrefixSize < kCompressMinBytes) << "chunk " << e.chunk_index;
    }
}

INSTANTIATE_TEST_SUITE_P(Formats, ReplayFramingOnDisk,
                         ::testing::Values(VTX::VtxFormat::FlatBuffers, VTX::VtxFormat::Protobuf),
                         [](const ::testing::TestParamInfo<VTX::VtxFormat>& info) {
                             return info.param == VTX::VtxFormat::FlatBuffers ? "FlatBuffers" : "Protobuf";
                         });
