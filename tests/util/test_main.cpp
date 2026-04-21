// Shared test main -- silences the VTX logger so test output stays clean,
// then runs GoogleTest as usual.

#include <gtest/gtest.h>
#include "vtx/common/vtx_logger.h"

namespace {

// Install a silent sink on the VTX logger so info/debug noise doesn't spam the
// test runner.  Tests that want to assert on logger output can pull from an
// override sink instead (see util/test_fixtures.h).
void InstallSilentLoggerSink()
{
    VTX::Logger::Instance().AddSink([](const VTX::Logger::Entry&) {});
}

} // namespace

int main(int argc, char** argv)
{
    InstallSilentLoggerSink();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
