/**
 * @file schema_validation_result.h
 * @brief Result types produced by the schema validator.
 *
 * @details A validation pass collects a flat list of issues (errors/warnings),
 * each tagged with the rule that raised it and the struct/field it concerns.
 * The result is intentionally a plain data carrier so callers (registry, tools,
 * tests) can inspect, format or surface it however they need.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "vtx/common/vtx_diagnostics.h"

namespace VTX {

    /**
     * @brief Severity of a single schema issue.
     * @details Aliased to the SDK-wide Severity so schema validation feeds the
     * same diagnostics model (VtxDiagnostic / ValidationReport) as every other
     * layer instead of carrying a parallel enum.
     */
    using SchemaIssueSeverity = Severity;

    /**
     * @brief One problem found while validating a schema.
     */
    struct SchemaIssue {
        SchemaIssueSeverity severity = SchemaIssueSeverity::Error;
        std::string rule;        ///< Name of the rule that raised it.
        std::string struct_name; ///< Owning struct (may be empty for global issues).
        std::string field_name;  ///< Owning field (may be empty for struct-level issues).
        std::string message;     ///< Human-readable description.
    };

    /**
     * @brief Aggregated outcome of a schema validation pass.
     */
    class SchemaValidationResult {
    public:
        void Add(SchemaIssue issue) { issues_.push_back(std::move(issue)); }

        void AddError(std::string rule, std::string struct_name, std::string field_name, std::string message) {
            issues_.push_back({SchemaIssueSeverity::Error, std::move(rule), std::move(struct_name),
                               std::move(field_name), std::move(message)});
        }

        void AddWarning(std::string rule, std::string struct_name, std::string field_name, std::string message) {
            issues_.push_back({SchemaIssueSeverity::Warning, std::move(rule), std::move(struct_name),
                               std::move(field_name), std::move(message)});
        }

        void Absorb(const SchemaValidationResult& other) {
            issues_.insert(issues_.end(), other.issues_.begin(), other.issues_.end());
        }

        const std::vector<SchemaIssue>& Issues() const { return issues_; }

        bool HasErrors() const {
            for (const auto& issue : issues_) {
                if (issue.severity == SchemaIssueSeverity::Error) {
                    return true;
                }
            }
            return false;
        }

        bool IsValid() const { return !HasErrors(); }

        size_t ErrorCount() const { return Count(SchemaIssueSeverity::Error); }
        size_t WarningCount() const { return Count(SchemaIssueSeverity::Warning); }

        std::string ToString() const {
            std::ostringstream os;
            for (const auto& issue : issues_) {
                os << '[' << (issue.severity == SchemaIssueSeverity::Error ? "ERROR" : "WARN") << "] ";
                os << '(' << issue.rule << ") ";
                if (!issue.struct_name.empty()) {
                    os << issue.struct_name;
                    if (!issue.field_name.empty()) {
                        os << '.' << issue.field_name;
                    }
                    os << ": ";
                }
                os << issue.message << '\n';
            }
            return os.str();
        }


        ValidationReport ToReport(const char* source_api = "ValidateSchema") const {
            ValidationReport report;
            for (const auto& issue : issues_) {
                VtxDiagnostic diagnostic;
                diagnostic.code = (issue.rule == "Json") ? VtxErrorCode::SchemaParseError : VtxErrorCode::SchemaInvalid;
                diagnostic.severity = issue.severity;
                diagnostic.message = issue.rule.empty() ? issue.message : ("(" + issue.rule + ") " + issue.message);
                diagnostic.entity_type = issue.struct_name;
                if (!issue.struct_name.empty() && !issue.field_name.empty()) {
                    diagnostic.field_path = issue.struct_name + "." + issue.field_name;
                } else if (!issue.field_name.empty()) {
                    diagnostic.field_path = issue.field_name;
                } else {
                    diagnostic.field_path = issue.struct_name;
                }
                diagnostic.source_api = source_api;
                report.Add(std::move(diagnostic));
            }
            return report;
        }

    private:
        size_t Count(SchemaIssueSeverity severity) const {
            size_t n = 0;
            for (const auto& issue : issues_) {
                if (issue.severity == severity) {
                    ++n;
                }
            }
            return n;
        }

        std::vector<SchemaIssue> issues_;
    };

} // namespace VTX
