#include "hostfw_error_publish.hpp"

#include <unistd.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace error = openpower::dump::phal::error;

TEST(HostfwErrorPublishTest, AttemptsEveryEntryAfterPublicationFailure)
{
    errl::ErrlHandle handle;
    handle.addEntry(std::make_unique<errl::ErrlEntry>(
        "terminal", errl::Level::Informational,
        std::unordered_map<std::string, std::string>{}));
    handle.addEntry(std::make_unique<errl::ErrlEntry>(
                        "observation", errl::Level::Error,
                        std::unordered_map<std::string, std::string>{}),
                    errl::ErrlHandle::EntryType::Observation);
    std::vector<std::string> attempts;
    int failures = 0;
    const auto pel = error::publishHostfwEntries(
        handle,
        [&](const errl::ErrlEntry& entry) -> uint32_t {
            attempts.push_back(entry.getMessage());
            if (entry.getMessage() == "terminal")
            {
                EXPECT_EQ(entry.getSeverity(), errl::Level::Informational);
                throw std::runtime_error("PEL unavailable");
            }
            EXPECT_EQ(entry.getSeverity(), errl::Level::Error);
            return 17;
        },
        [&](const errl::ErrlEntry&, const char*) { ++failures; });
    EXPECT_EQ(attempts, (std::vector<std::string>{"terminal", "observation"}));
    EXPECT_EQ(failures, 1);
    EXPECT_EQ(pel, 17U);
    EXPECT_TRUE(handle.hasTerminalErrors());
}

TEST(HostfwErrorPublishTest, LaterFailureRetainsFirstSuccessfullyPublishedPel)
{
    errl::ErrlHandle handle;
    for (const auto* message : {"first", "second", "failed"})
    {
        handle.addEntry(std::make_unique<errl::ErrlEntry>(
            message, errl::Level::Error,
            std::unordered_map<std::string, std::string>{}));
    }
    int publications = 0;
    int failures = 0;
    const auto pel = error::publishHostfwEntries(
        handle,
        [&](const errl::ErrlEntry& entry) -> uint32_t {
            ++publications;
            if (entry.getMessage() == "failed")
            {
                throw std::runtime_error("PEL unavailable");
            }
            return publications;
        },
        [&](const errl::ErrlEntry&, const char*) { ++failures; });
    EXPECT_EQ(pel, 1U);
    EXPECT_EQ(publications, 3);
    EXPECT_EQ(failures, 1);
}

TEST(HostfwErrorPublishTest, ZeroPelIdDoesNotPreventPublishingLaterEntries)
{
    errl::ErrlHandle handle;
    for (const auto* message : {"zero", "published"})
    {
        handle.addEntry(std::make_unique<errl::ErrlEntry>(
            message, errl::Level::Error,
            std::unordered_map<std::string, std::string>{}));
    }
    int publications = 0;
    int failures = 0;
    const auto pel = error::publishHostfwEntries(
        handle,
        [&](const errl::ErrlEntry& entry) -> uint32_t {
            ++publications;
            return entry.getMessage() == "zero" ? 0U : 23U;
        },
        [&](const errl::ErrlEntry&, const char*) { ++failures; });
    EXPECT_EQ(pel, 23U);
    EXPECT_EQ(publications, 2);
    EXPECT_EQ(failures, 1);
}

TEST(HostfwErrorPublishTest, PreservesWireAttachmentBytesSubtypeAndVersion)
{
    const std::vector<uint8_t> wire{0xEB, 0xAD, 0, 4, 1, 2, 3, 4};
    errl::ErrlHandle handle;
    handle.addEntry(std::make_unique<errl::ErrlEntry>(
                        "SBE FFDC", errl::Level::Error,
                        std::unordered_map<std::string, std::string>{},
                        std::nullopt, std::nullopt,
                        std::vector<errl::FfdcAttachment>{
                            {.subType = 0xCC, .version = 1, .data = wire}}),
                    errl::ErrlHandle::EntryType::Observation);
    int publications = 0;
    const auto pel = error::publishHostfwEntries(
        handle,
        [&](const errl::ErrlEntry& entry) -> uint32_t {
            ++publications;
            EXPECT_EQ(entry.getSeverity(), errl::Level::Error);
            EXPECT_TRUE(entry.getFfdcFiles());
            const auto& attachment = entry.getFfdcFiles()->front().first;
            EXPECT_EQ(attachment.subType, 0xCC);
            EXPECT_EQ(attachment.version, 1);
            std::vector<uint8_t> actual(wire.size());
            EXPECT_EQ(pread(attachment.fd, actual.data(), actual.size(), 0),
                      static_cast<ssize_t>(wire.size()));
            EXPECT_EQ(actual, wire);
            return 18;
        },
        [](const errl::ErrlEntry&, const char* message) { FAIL() << message; });
    EXPECT_EQ(pel, 18U);
    EXPECT_EQ(publications, 1);
    EXPECT_FALSE(handle.hasTerminalErrors());
}

TEST(HostfwErrorPublishTest, ErrorObservationRetainsCalloutActionsAndSuccess)
{
    const nlohmann::json original = nlohmann::json::array(
        {{{"EntityPath", {1, 2, 3}},
          {"Priority", "H"},
          {"Deconfigured", true},
          {"Guarded", true},
          {"GuardType", "FATAL"}}});
    errl::ErrlHandle handle;
    handle.addEntry(
        std::make_unique<errl::ErrlEntry>(
            "SBE observation", errl::Level::Error,
            std::unordered_map<std::string, std::string>{}, original),
        errl::ErrlHandle::EntryType::Observation);
    int publications = 0;
    const auto pel = error::publishHostfwEntries(
        handle,
        [&](const errl::ErrlEntry& entry) -> uint32_t {
            ++publications;
            EXPECT_EQ(entry.getSeverity(), errl::Level::Error);
            EXPECT_TRUE(entry.getCallout());
            if (!entry.getCallout())
            {
                return 0;
            }
            EXPECT_EQ(*entry.getCallout(), original);
            EXPECT_TRUE(entry.getFfdcFiles());
            if (!entry.getFfdcFiles() || entry.getFfdcFiles()->empty())
            {
                return 0;
            }
            const auto& attachment = entry.getFfdcFiles()->front().first;
            EXPECT_EQ(attachment.format, errl::FFDCFormat::JSON);
            EXPECT_EQ(attachment.subType, 0xCA);
            std::string callouts(original.dump(2).size(), '\0');
            EXPECT_EQ(pread(attachment.fd, callouts.data(), callouts.size(), 0),
                      static_cast<ssize_t>(callouts.size()));
            EXPECT_EQ(nlohmann::json::parse(callouts), original);
            return 19;
        },
        [](const errl::ErrlEntry&, const char* message) { FAIL() << message; });
    EXPECT_EQ(pel, 19U);
    EXPECT_EQ(publications, 1);
    EXPECT_FALSE(handle.hasTerminalErrors());
}
