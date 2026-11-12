#pragma once

#include "dump_utils.hpp"
#include "sbe_consts.hpp"

#include <systemd/sd-event.h>

#include <com/ibm/Dump/Create/common.hpp>
#include <com/ibm/Dump/Entry/Resource/common.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/bus/match.hpp>
#include <xyz/openbmc_project/Common/Progress/common.hpp>
#include <xyz/openbmc_project/Dump/Entry/System/common.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace openpower::dump
{

using PropertyMap = std::map<std::string, std::variant<uint32_t, std::string>>;
using InterfaceMap = std::map<std::string, PropertyMap>;

/** Staging directory where raw MPIPL (SYSDUMP) and resource (RSCDUMP) dump
 *  files are written.  Must match PLDM's dump transfer destination. */
constexpr auto SYSTEM_DUMP_STAGING_PATH =
    "/var/lib/phosphor-debug-collector/tmp";

/** Path where packaged dump directories are placed. */
constexpr auto SYSTEM_DUMP_OUT_PATH =
    "/var/lib/phosphor-debug-collector/opdump";

/** D-Bus base path of the dump manager's entries. Both System and Resource
 *  dump entries are created under it. */
constexpr auto SYSTEM_DUMP_ENTRY_BASE =
    "/xyz/openbmc_project/dump/system/entry";

/**
 * @class DumpMonitor
 * @brief Monitors DBus signals for dump creation and handles them.
 */
class DumpMonitor
{
  public:
    /**
     * @brief Constructor for DumpMonitor.
     * Initializes the event loop, DBus signal match, and system dump directory
     * watch.
     */
    DumpMonitor();

    ~DumpMonitor();

    DumpMonitor(const DumpMonitor&) = delete;
    DumpMonitor& operator=(const DumpMonitor&) = delete;
    DumpMonitor(DumpMonitor&&) = delete;
    DumpMonitor& operator=(DumpMonitor&&) = delete;

    /**
     * @brief Runs the monitor to listen for DBus and filesystem events.
     *
     * @return The event loop return code.
     */
    int run();

  private:
    struct EventDeleter
    {
        void operator()(sd_event* event) const noexcept
        {
            sd_event_unref(event);
        }
    };

    /* @brief Event loop shared by DBus and inotify */
    std::unique_ptr<sd_event, EventDeleter> event;

    /* @brief sdbusplus handler for a bus to use */
    sdbusplus::bus_t bus;

    /* @brief Monitores dump interfaces */
    const std::vector<std::string> monitoredInterfaces = {
        "com.ibm.Dump.Entry.Hardware", "com.ibm.Dump.Entry.Hostboot",
        "com.ibm.Dump.Entry.SBE", "xyz.openbmc_project.Dump.Entry.System"};

    /* @brief InterfaceAdded match */
    sdbusplus::bus::match_t match;

    /* @brief File descriptor for the inotify instance */
    int inotifyFd = -1;

    /* @brief Watch descriptor for the temporary system dump directory */
    int inotifyWatchDescriptor = -1;

    /* @brief Event source used to dispatch inotify events */
    sd_event_source* inotifyEventSource = nullptr;

    /**
     * @brief Handles the received DBus signal for dump creation.
     * @param[in] msg - The DBus message received.
     */
    void handleDBusSignal(sdbusplus::message_t& msg);

    /**
     * @brief Adds the temporary system dump directory to the event loop.
     */
    void setupInotifyWatch();

    /**
     * @brief Handles readiness notifications for the inotify descriptor.
     *
     * @return Zero so the event source remains enabled.
     */
    static int inotifyCallback(sd_event_source* source, int fd,
                               uint32_t revents, void* userdata);

    /**
     * @brief Reads the 7-byte dump type tag from offset 0x18 of the raw
     *        staging file (PAPR FILE Directory, "Dump File Name" field) and
     *        returns it as a string, e.g. "RSCDUMP" or "SYSDUMP".
     *        Returns an empty string on any read error.
     * @param[in] filePath - Path to the raw staging file.
     */
    static std::string readDumpTypeTag(const std::filesystem::path& filePath);

    /**
     * @brief Correlates a raw dump file written into the staging directory
     *        with an in-progress D-Bus entry and forks opdreport to package it.
     *        The dump type (System vs Resource) is determined by reading the
     *        7-byte type tag at offset 0x18 of the file header.
     * @param[in] filePath - Path reported by inotify.
     */
    void handleDumpFile(const std::filesystem::path& filePath);

    /**
     * @brief Finds an in-progress dump entry of the given type via
     *        ObjectMapper.
     *
     * Queries the subtree rooted at @p entryRoot for objects implementing
     * @p dumpInterface that are currently InProgress.  If multiple entries
     * are in progress, logs an error and returns the first in lexical
     * object-path order so that the dump is not discarded.
     *
     * @param[in] entryRoot      D-Bus subtree root to search (e.g.
     *                           "/xyz/openbmc_project/dump/system/entry").
     * @param[in] dumpInterface  D-Bus interface to filter on (e.g.
     *                           "xyz.openbmc_project.Dump.Entry.System").
     * @return The selected entry object path, or std::nullopt if none found.
     */
    std::optional<std::string> findInProgressDump(
        const std::string& entryRoot, const std::string& dumpInterface);

    /**
     * @brief Asks the dump manager to create an entry in NotStarted state,
     *        for a raw dump file that no dump request preceded.
     *
     * @param[in] isResource - true for a Resource dump, false for System.
     * @return The object path of the new entry, or std::nullopt on failure.
     */
    std::optional<std::string> createNotStartedDump(bool isResource);

    /**
     * @brief Deletes a dump entry through its Object.Delete interface. Errors
     *        are logged and not thrown.
     *
     * @param[in] path - Object path of the entry.
     */
    void deleteDumpEntry(const std::string& path);

    /**
     * @brief Checks if the dump creation is in progress.
     * @param[in] interfaces - The map of interfaces and their properties.
     * @return True if the dump is in progress, false otherwise.
     */
    inline bool isInProgress(const InterfaceMap& interfaces)
    {
        using namespace sdbusplus::common::xyz::openbmc_project::common;
        auto progressIt = interfaces.find(Progress::interface);
        if (progressIt != interfaces.end())
        {
            auto statusIt = progressIt->second.find("Status");
            if (statusIt != progressIt->second.end())
            {
                std::string status = std::get<std::string>(statusIt->second);
                return status == Progress::convertOperationStatusToString(
                                     Progress::OperationStatus::InProgress);
            }
        }
        return false;
    }

    /**
     * @brief Executes the script to collect the dump.
     * @param[in] path - The object path of the dump entry.
     * @param[in] properties - The properties of the dump entry.
     */
    void executeCollectionScript(const sdbusplus::message::object_path& path,
                                 const PropertyMap& properties);

    /**
     * @brief Updates the progress status of the dump.
     * @param[in] path - The object path of the dump entry.
     * @param[in] status - The status to be set.
     */
    void updateProgressStatus(const std::string& path,
                              const std::string& status);

    /**
     * @brief Gets the dump type from the dump ID.
     * @param[in] id - The dump ID.
     * @return The dump type.
     */
    inline uint32_t getDumpTypeFromId(uint32_t id)
    {
        using namespace openpower::dump::SBE;
        uint8_t type = (id >> 28) & 0xF;
        if (type == 0)
        {
            return SBE_DUMP_TYPE_HARDWARE;
        }
        else if (type == 2)
        {
            return SBE_DUMP_TYPE_HOSTBOOT;
        }
        else if (type == 3)
        {
            return SBE_DUMP_TYPE_SBE;
        }
        else if (type == 4)
        {
            return SBE_DUMP_TYPE_MSBE;
        }
        return 0;
    }

    /**
     * @brief Initiates the dump collection process based on the given interface
     *        and properties.
     *
     * @param[in] path The object path of the dump entry.
     * @param[in] intf The interface type of the dump entry.
     * @param[in] properties The properties of the dump entry.
     */
    void initiateDumpCollection(const std::string& path,
                                const std::string& intf,
                                const PropertyMap& properties);

    /**
     * @brief Initiates a memory-preserving reboot by starting the
     *        appropriate systemd unit.
     */
    void startMpReboot(const sdbusplus::message::object_path& objectPath);
};

} // namespace openpower::dump
