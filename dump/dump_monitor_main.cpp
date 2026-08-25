#include "dump_monitor.hpp"

#include <exception>
#include <iostream>

int main()
{
    try
    {
        openpower::dump::DumpMonitor monitor;
        return monitor.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "openpower-dump-monitor: fatal: " << e.what() << "\n";
        return 1;
    }
}
