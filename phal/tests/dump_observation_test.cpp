#include "chipop_iface.hpp"
#include "error_iface.hpp"
#include "hostfw_error_publish.hpp"

#include <dump.H>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

namespace
{
std::vector<uint8_t> payload;
errl::ErrlHandleOpt diagnostics;
int backendCalls = 0;
int publications = 0;
int publicationFailures = 0;
bool publicationUnavailable = false;

errl::ErrlHandleOpt observation(bool terminal = false)
{
    errl::ErrlHandle handle;
    if (terminal)
    {
        handle.addEntry(std::make_unique<errl::ErrlEntry>(
            "Failed dump operation", errl::Level::Informational,
            std::unordered_map<std::string, std::string>{}));
    }
    handle.addEntry(std::make_unique<errl::ErrlEntry>(
                        "SBE observation", errl::Level::Error,
                        std::unordered_map<std::string, std::string>{}),
                    errl::ErrlHandle::EntryType::Observation);
    return errl::toOptional(std::move(handle));
}

class DumpObservationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        payload = {0x00, 0x41, 0xFF, 0x80};
        diagnostics.reset();
        backendCalls = publications = publicationFailures = 0;
        publicationUnavailable = false;
    }
};
} // namespace

namespace hostfw::dump
{
errl::ErrlHandleOpt getDump(TARGETING::ConstTargetPtr, uint8_t, uint8_t,
                            uint8_t, std::vector<uint8_t>& bytes)
{
    ++backendCalls;
    bytes = payload;
    return std::move(diagnostics);
}
} // namespace hostfw::dump

namespace openpower::dump::phal::error
{
uint32_t commitHostfwError(errl::ErrlHandleOpt&& errors)
{
    if (!errors)
        return 0;
    return publishHostfwEntries(
        *errors,
        [](const errl::ErrlEntry&) -> uint32_t {
            ++publications;
            if (publicationUnavailable)
                throw std::runtime_error("PEL unavailable");
            return 42;
        },
        [](const errl::ErrlEntry&, const char*) { ++publicationFailures; });
}
} // namespace openpower::dump::phal::error

namespace chipop = openpower::dump::phal::chipop;
namespace error = openpower::dump::phal::error;

TEST_F(DumpObservationTest, SuccessfulDumpWithoutDiagnosticsRetainsPayload)
{
    auto data = chipop::getDump(nullptr, 1, 1, 0);
    EXPECT_EQ(std::vector<uint8_t>(data.bytes().begin(), data.bytes().end()),
              payload);
    EXPECT_EQ(backendCalls, 1);
    EXPECT_EQ(publications, 0);
}

TEST_F(DumpObservationTest, SuccessfulDumpBytesSurviveHighSeverityObservations)
{
    diagnostics = observation();
    auto data = chipop::getDump(nullptr, 1, 1, 0);
    EXPECT_EQ(std::vector<uint8_t>(data.bytes().begin(), data.bytes().end()),
              payload);
    EXPECT_EQ(backendCalls, 1);
    EXPECT_EQ(publications, 1);
}

TEST_F(DumpObservationTest,
       LowSeverityTerminalErrorStillFailsAfterPublishingAllEntries)
{
    diagnostics = observation(true);
    EXPECT_THROW(chipop::getDump(nullptr, 1, 1, 0), chipop::ChipOpError);
    EXPECT_EQ(backendCalls, 1);
    EXPECT_EQ(publications, 2);
}

TEST_F(DumpObservationTest, PublicationFailureDoesNotInvalidateCollectedDump)
{
    diagnostics = observation();
    publicationUnavailable = true;
    auto data = chipop::getDump(nullptr, 1, 1, 0);
    EXPECT_EQ(std::vector<uint8_t>(data.bytes().begin(), data.bytes().end()),
              payload);
    EXPECT_EQ(publicationFailures, 1);
}

TEST_F(DumpObservationTest, PublicationFailureDoesNotHideTerminalError)
{
    diagnostics = observation(true);
    publicationUnavailable = true;
    EXPECT_THROW(chipop::getDump(nullptr, 1, 1, 0), chipop::ChipOpError);
    EXPECT_EQ(backendCalls, 1);
    EXPECT_EQ(publications, 2);
    EXPECT_EQ(publicationFailures, 2);
}

TEST_F(DumpObservationTest,
       EmptyPayloadRemainsCollectionFailureWithObservations)
{
    diagnostics = observation();
    payload.clear();
    EXPECT_THROW(chipop::getDump(nullptr, 1, 1, 0), chipop::ChipOpError);
    EXPECT_EQ(publications, 1);
}

TEST_F(DumpObservationTest, PublishingLaterTerminalErrorRetainsExistingDumpFile)
{
    char filename[] = "/tmp/dump-observation-XXXXXX";
    const int fd = mkstemp(filename);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(write(fd, payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));
    close(fd);
    struct Cleanup
    {
        const char* file;
        ~Cleanup()
        {
            unlink(file);
        }
    } cleanup{filename};
    uint32_t pelId = 0;
    EXPECT_FALSE(
        error::commitHostfwErrorAndCheckFailure(observation(), &pelId));
    EXPECT_EQ(pelId, 42U);
    EXPECT_TRUE(
        error::commitHostfwErrorAndCheckFailure(observation(true), &pelId));
    std::ifstream file(filename, std::ios::binary);
    const std::vector<uint8_t> remaining{std::istreambuf_iterator<char>(file),
                                         std::istreambuf_iterator<char>()};
    EXPECT_EQ(remaining, payload);
    EXPECT_EQ(publications, 3);
}
