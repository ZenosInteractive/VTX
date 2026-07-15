/**
 * @file vtx_diagnostics.h
 * @brief Structured diagnostics shared across the SDK.
 * @author Zenos Interactive
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VTX {

    /**
     * @brief Severity of a diagnostic. Only Error makes a result/pass invalid.
     */
    enum class Severity { Warning, Error };

    inline std::string_view ToString(Severity severity) {
        switch (severity) {
        case Severity::Warning:
            return "Warning";
        case Severity::Error:
            return "Error";
        }
        return "Unknown";
    }

    /**
     * @brief Stable, machine-checkable failure code.
     * @details Grouped by area; new codes are appended (never renumbered) so
     * consumers can switch on them across SDK versions.
     */
    enum class VtxErrorCode {
        None = 0,

        InvalidArgument,
        NotFound,
        Internal,

        SchemaParseError,
        SchemaInvalid,
        SchemaMissing,

        EntityTypeUnresolved,
        BucketUnresolved,
        FieldIndexOutOfRange,
        TypeMismatch,
        ContainerMismatch,
        DuplicateUniqueId,

        OutputPathInvalid,
        WriteFailed,

        GameTimeRejected,

        ReplayOpenFailed,
        ReplayNotReady,
    };

    inline std::string_view ToString(VtxErrorCode code) {
        switch (code) {
        case VtxErrorCode::None:
            return "None";
        case VtxErrorCode::InvalidArgument:
            return "InvalidArgument";
        case VtxErrorCode::NotFound:
            return "NotFound";
        case VtxErrorCode::Internal:
            return "Internal";
        case VtxErrorCode::SchemaParseError:
            return "SchemaParseError";
        case VtxErrorCode::SchemaInvalid:
            return "SchemaInvalid";
        case VtxErrorCode::SchemaMissing:
            return "SchemaMissing";
        case VtxErrorCode::EntityTypeUnresolved:
            return "EntityTypeUnresolved";
        case VtxErrorCode::BucketUnresolved:
            return "BucketUnresolved";
        case VtxErrorCode::FieldIndexOutOfRange:
            return "FieldIndexOutOfRange";
        case VtxErrorCode::TypeMismatch:
            return "TypeMismatch";
        case VtxErrorCode::ContainerMismatch:
            return "ContainerMismatch";
        case VtxErrorCode::DuplicateUniqueId:
            return "DuplicateUniqueId";
        case VtxErrorCode::OutputPathInvalid:
            return "OutputPathInvalid";
        case VtxErrorCode::WriteFailed:
            return "WriteFailed";
        case VtxErrorCode::GameTimeRejected:
            return "GameTimeRejected";
        case VtxErrorCode::ReplayOpenFailed:
            return "ReplayOpenFailed";
        case VtxErrorCode::ReplayNotReady:
            return "ReplayNotReady";
        }
        return "Unknown";
    }

    /**
     * @brief One structured diagnostic (error or warning).
     * @details Aggregate type: call sites set only the fields they know.
     */
    struct VtxDiagnostic {
        VtxErrorCode code = VtxErrorCode::None;
        Severity severity = Severity::Error;
        std::string message;

        int32_t frame_index = -1;
        std::string bucket;
        std::string unique_id;
        std::string entity_type;
        std::string field_path;
        std::string expected_type;
        std::string expected_container;
        std::string provided_type;
        std::string provided_container;
        std::string source_api;

        bool IsError() const { return severity == Severity::Error; }

        std::string ToString() const {
            std::ostringstream os;
            os << '[' << VTX::ToString(severity) << "] " << VTX::ToString(code);
            if (!source_api.empty()) {
                os << " {" << source_api << '}';
            }
            os << ": " << message;

            std::ostringstream loc;
            const auto add = [&](const char* key, const std::string& value) {
                if (!value.empty()) {
                    loc << ' ' << key << '=' << value;
                }
            };
            if (frame_index >= 0) {
                loc << " frame=" << frame_index;
            }
            add("bucket", bucket);
            add("id", unique_id);
            add("entityType", entity_type);
            add("field", field_path);
            add("expected", expected_type);
            add("expectedContainer", expected_container);
            add("provided", provided_type);
            add("providedContainer", provided_container);

            const std::string located = loc.str();
            if (!located.empty()) {
                os << " |" << located;
            }
            return os.str();
        }
    };

    inline std::ostream& operator<<(std::ostream& os, const VtxDiagnostic& diagnostic) {
        return os << diagnostic.ToString();
    }

    using VtxError = VtxDiagnostic;
    using VtxWarning = VtxDiagnostic;

    /// Placeholder value type for status-only results (no payload).
    struct Unit {};

    /**
     * @brief A produced value plus structured error/warnings.
     * */
    template <class T>
    struct VtxResult {
        bool ok = false;
        T value {};
        VtxError error {};
        std::vector<VtxWarning> warnings;

        explicit operator bool() const { return ok; }

        static VtxResult Success(T value, std::vector<VtxWarning> warnings = {}) {
            VtxResult result;
            result.ok = true;
            result.value = std::move(value);
            result.warnings = std::move(warnings);
            return result;
        }

        static VtxResult Failure(VtxError error, std::vector<VtxWarning> warnings = {}) {
            VtxResult result;
            result.ok = false;
            result.error = std::move(error);
            result.warnings = std::move(warnings);
            return result;
        }
    };

    /// Status-only result.
    using VtxStatus = VtxResult<Unit>;

    /**
     * @brief Aggregated diagnostics produced by a validation pass.
     */
    class ValidationReport {
    public:
        void Add(VtxDiagnostic diagnostic) { diagnostics_.push_back(std::move(diagnostic)); }

        void Absorb(const ValidationReport& other) {
            diagnostics_.insert(diagnostics_.end(), other.diagnostics_.begin(), other.diagnostics_.end());
        }

        const std::vector<VtxDiagnostic>& Diagnostics() const { return diagnostics_; }

        bool HasErrors() const {
            for (const auto& diagnostic : diagnostics_) {
                if (diagnostic.severity == Severity::Error) {
                    return true;
                }
            }
            return false;
        }

        bool ok() const { return !HasErrors(); }
        explicit operator bool() const { return ok(); }

        size_t ErrorCount() const { return Count(Severity::Error); }
        size_t WarningCount() const { return Count(Severity::Warning); }

        std::vector<VtxDiagnostic> Errors() const { return Filter(Severity::Error); }
        std::vector<VtxDiagnostic> Warnings() const { return Filter(Severity::Warning); }

        std::string ToString() const {
            std::ostringstream os;
            for (const auto& diagnostic : diagnostics_) {
                os << diagnostic.ToString() << '\n';
            }
            return os.str();
        }

    private:
        size_t Count(Severity severity) const {
            size_t n = 0;
            for (const auto& diagnostic : diagnostics_) {
                if (diagnostic.severity == severity) {
                    ++n;
                }
            }
            return n;
        }

        std::vector<VtxDiagnostic> Filter(Severity severity) const {
            std::vector<VtxDiagnostic> out;
            for (const auto& diagnostic : diagnostics_) {
                if (diagnostic.severity == severity) {
                    out.push_back(diagnostic);
                }
            }
            return out;
        }

        std::vector<VtxDiagnostic> diagnostics_;
    };

} // namespace VTX
