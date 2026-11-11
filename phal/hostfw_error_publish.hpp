#pragma once

#include <errl_handle.H>

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

namespace openpower::dump::phal::error
{
/**
 * @brief Publish every HostFW entry without changing the collection outcome.
 *
 * The publisher receives each entry with its native severity and requested
 * callout actions. Observation classification does not suppress those actions.
 * A failed publication is reported and later entries are still attempted. The
 * first successful PEL ID is retained for dump tracking. Entries are ordered
 * terminal-first, so this is the terminal entry's PEL when it was published.
 */
template <typename Publisher, typename FailureReporter>
uint32_t publishHostfwEntries(const errl::ErrlHandle& errors,
                              Publisher&& publish, FailureReporter&& failed)
{
    uint32_t firstPelId = 0;
    for (const auto& entry : errors.getEntries())
    {
        try
        {
            const auto id = publish(*entry);
            if (id == 0)
            {
                throw std::runtime_error("PEL service returned a zero PEL ID");
            }
            if (firstPelId == 0)
            {
                firstPelId = id;
            }
        }
        catch (const std::exception& exception)
        {
            failed(*entry, exception.what());
        }
        catch (...)
        {
            failed(*entry, "Unknown PEL publication failure");
        }
    }
    return firstPelId;
}
} // namespace openpower::dump::phal::error
