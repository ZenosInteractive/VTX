// Tests for the structured diagnostics foundation and the independently
// callable validation passes (ValidateSchema / ValidateEntity / ValidateFrame).

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/vtx_diagnostics.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_validation.h"

#include "util/test_fixtures.h"

namespace {

    std::string Slurp(const std::string& path) {
        std::ifstream file(path);
        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    bool HasCode(const VTX::ValidationReport& report, VTX::VtxErrorCode code) {
        for (const auto& d : report.Diagnostics()) {
            if (d.code == code) {
                return true;
            }
        }
        return false;
    }

    // Player layout (test_schema.json): string<=2, int32<=3, float<=2.
    VTX::PropertyContainer MakePlayer(int32_t team) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0; // Player
        pc.string_properties = {"id", "name"};
        pc.int32_properties = {team, 0, 0};
        pc.float_properties = {100.0f, 50.0f};
        return pc;
    }

    VTX::SchemaRegistry LoadSchema() {
        VTX::SchemaRegistry registry;
        registry.LoadFromJson(VtxTest::FixturePath("test_schema.json"));
        return registry;
    }

} // namespace

// ---------------------------------------------------------------------------
// Foundation: VtxResult / VtxStatus / ValidationReport / VtxDiagnostic
// ---------------------------------------------------------------------------

TEST(Diagnostics, VtxResultSuccessAndFailure) {
    const auto ok = VTX::VtxResult<int>::Success(42);
    EXPECT_TRUE(ok.ok);
    EXPECT_TRUE(static_cast<bool>(ok));
    EXPECT_EQ(ok.value, 42);
    EXPECT_EQ(ok.error.code, VTX::VtxErrorCode::None);

    VTX::VtxError err;
    err.code = VTX::VtxErrorCode::InvalidArgument;
    err.message = "bad";
    const auto fail = VTX::VtxResult<int>::Failure(err);
    EXPECT_FALSE(fail.ok);
    EXPECT_FALSE(static_cast<bool>(fail));
    EXPECT_EQ(fail.error.code, VTX::VtxErrorCode::InvalidArgument);
}

TEST(Diagnostics, VtxStatusCarriesWarnings) {
    VTX::VtxWarning w;
    w.severity = VTX::Severity::Warning;
    w.code = VTX::VtxErrorCode::SchemaMissing;
    auto status = VTX::VtxStatus::Success(VTX::Unit {}, {w});
    EXPECT_TRUE(status.ok);
    ASSERT_EQ(status.warnings.size(), 1u);
    EXPECT_EQ(status.warnings[0].code, VTX::VtxErrorCode::SchemaMissing);
}

TEST(Diagnostics, ValidationReportCountsAndRendering) {
    VTX::ValidationReport report;
    EXPECT_TRUE(report.ok());

    VTX::VtxDiagnostic error;
    error.severity = VTX::Severity::Error;
    error.code = VTX::VtxErrorCode::TypeMismatch;
    error.message = "boom";
    error.field_path = "Player.Health";
    report.Add(error);

    VTX::VtxDiagnostic warning;
    warning.severity = VTX::Severity::Warning;
    warning.code = VTX::VtxErrorCode::SchemaMissing;
    warning.message = "advisory";
    report.Add(warning);

    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(report.HasErrors());
    EXPECT_EQ(report.ErrorCount(), 1u);
    EXPECT_EQ(report.WarningCount(), 1u);
    EXPECT_EQ(report.Errors().size(), 1u);
    EXPECT_EQ(report.Warnings().size(), 1u);

    const std::string rendered = report.ToString();
    EXPECT_NE(rendered.find("TypeMismatch"), std::string::npos);
    EXPECT_NE(rendered.find("Player.Health"), std::string::npos);
    EXPECT_NE(rendered.find("boom"), std::string::npos);
}

// ---------------------------------------------------------------------------
// ValidateSchema
// ---------------------------------------------------------------------------

TEST(Diagnostics, ValidateSchemaAcceptsValidDocument) {
    const std::string json = Slurp(VtxTest::FixturePath("test_schema.json"));
    ASSERT_FALSE(json.empty());
    const auto report = VTX::ValidateSchema(json);
    EXPECT_TRUE(report.ok()) << report.ToString();
}

TEST(Diagnostics, ValidateSchemaReportsParseError) {
    const auto report = VTX::ValidateSchema("{ this is not valid json ");
    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(HasCode(report, VTX::VtxErrorCode::SchemaParseError));
}

TEST(Diagnostics, ValidateSchemaReportsMissingMapping) {
    const auto report = VTX::ValidateSchema(R"({"version":"1.0.0"})");
    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(HasCode(report, VTX::VtxErrorCode::SchemaInvalid));
}

// ---------------------------------------------------------------------------
// ValidateEntity
// ---------------------------------------------------------------------------

TEST(Diagnostics, ValidateEntityAcceptsValidEntity) {
    const auto schema = LoadSchema();
    const auto report = VTX::ValidateEntity(MakePlayer(1), schema);
    EXPECT_TRUE(report.ok()) << report.ToString();
}

TEST(Diagnostics, ValidateEntityRejectsUnresolvedType) {
    const auto schema = LoadSchema();
    VTX::PropertyContainer pc = MakePlayer(1);
    pc.entity_type_id = -1;

    VTX::EntityLocation where;
    where.frame_index = 3;
    where.bucket = "entity";
    where.unique_id = "p7";
    const auto report = VTX::ValidateEntity(pc, schema, where);

    ASSERT_FALSE(report.ok());
    ASSERT_EQ(report.Diagnostics().size(), 1u);
    const auto& d = report.Diagnostics()[0];
    EXPECT_EQ(d.code, VTX::VtxErrorCode::EntityTypeUnresolved);
    EXPECT_EQ(d.severity, VTX::Severity::Error);
    EXPECT_EQ(d.frame_index, 3);
    EXPECT_EQ(d.bucket, "entity");
    EXPECT_EQ(d.unique_id, "p7");
    EXPECT_EQ(d.source_api, "ValidateEntity");
}

TEST(Diagnostics, ValidateEntityRejectsOversizedArray) {
    const auto schema = LoadSchema();
    VTX::PropertyContainer pc = MakePlayer(1);
    pc.int32_properties = {1, 2, 3, 4, 5}; // schema allows at most 3

    const auto report = VTX::ValidateEntity(pc, schema);
    ASSERT_TRUE(HasCode(report, VTX::VtxErrorCode::FieldIndexOutOfRange));
    const auto& d = report.Diagnostics()[0];
    EXPECT_EQ(d.entity_type, "Player");
    EXPECT_EQ(d.expected_type, "Int32");
}

// ---------------------------------------------------------------------------
// ValidateFrame
// ---------------------------------------------------------------------------

TEST(Diagnostics, ValidateFrameAcceptsCleanFrame) {
    const auto schema = LoadSchema();
    VTX::Frame frame;
    auto& bucket = frame.CreateBucket("entity");
    bucket.unique_ids = {"a", "b"};
    bucket.entities = {MakePlayer(1), MakePlayer(2)};

    const auto report = VTX::ValidateFrame(frame, schema, 0);
    EXPECT_TRUE(report.ok()) << report.ToString();
}

TEST(Diagnostics, ValidateFrameDetectsDuplicateUniqueId) {
    const auto schema = LoadSchema();
    VTX::Frame frame;
    auto& bucket = frame.CreateBucket("entity");
    bucket.unique_ids = {"dup", "dup"};
    bucket.entities = {MakePlayer(1), MakePlayer(2)};

    const auto report = VTX::ValidateFrame(frame, schema, 5);
    ASSERT_TRUE(HasCode(report, VTX::VtxErrorCode::DuplicateUniqueId));
    bool found = false;
    for (const auto& d : report.Diagnostics()) {
        if (d.code == VTX::VtxErrorCode::DuplicateUniqueId) {
            EXPECT_EQ(d.bucket, "entity");
            EXPECT_EQ(d.unique_id, "dup");
            EXPECT_EQ(d.frame_index, 5);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(Diagnostics, ValidateFramePropagatesEntityContext) {
    const auto schema = LoadSchema();
    VTX::Frame frame;
    auto& bucket = frame.CreateBucket("entity");
    VTX::PropertyContainer bad = MakePlayer(1);
    bad.entity_type_id = -1;
    bucket.unique_ids = {"ghost"};
    bucket.entities = {bad};

    const auto report = VTX::ValidateFrame(frame, schema, 9);
    ASSERT_TRUE(HasCode(report, VTX::VtxErrorCode::EntityTypeUnresolved));
    const auto& d = report.Diagnostics()[0];
    EXPECT_EQ(d.frame_index, 9);
    EXPECT_EQ(d.bucket, "entity");
    EXPECT_EQ(d.unique_id, "ghost");
}
