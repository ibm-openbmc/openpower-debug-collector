#pragma once

#include "dump_utils.hpp"
#include "sbe_consts.hpp"

#include <systemd/sd-event.h>

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

/** Path where the Hostboot/MPIPL agent writes the raw dump file. */
constexpr auto MPIPL_STAGING_PATH = "/var/lib/phosphor-debug-collector/tmp";

/** Path where packaged dump directories are placed. */
constexpr auto MPIPL_DUMP_OUT_PATH = "/var/lib/phosphor-debug-collector/opdump";

/** D-Bus base path for system dump entries. */
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
     * @brief Correlates a new dump file with an in-progress system dump entry
     *        and forks opdreport --mpipl-src to package it.
     * @param[in] filePath - Path reported by inotify.
     */
    void handleSystemDumpFile(const std::filesystem::path& filePath);

    /**
     * @brief Finds an in-progress system dump entry via ObjectMapper.
     *
     * If multiple entries are in progress, logs an error and returns the first
     * entry in lexical object-path order so that the dump is not discarded.
     *
     * @return The selected entry object path, or std::nullopt if none found.
     */
    std::optional<std::string> findInProgressSystemDump();

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
