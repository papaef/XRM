/*
 * Copyright (C) 2019-2020, Xilinx Inc - All rights reserved
 * Xilinx Resouce Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <boost/asio.hpp>
#include "xrm_version.h"
#include "xrm_command_registry.hpp"
#include "xrm_tcp_server.hpp"
#include "xrm_system.hpp"

#include <syslog.h>

using boost::asio::ip::tcp;

int main(int argc, char* argv[]) {
    std::ignore = argc;
    std::ignore = argv;

    xrm::system* sys = NULL;
    xrm::commandRegistry* registry = NULL;
    boost::asio::io_service* ioService = NULL;
    xrm::server* serv = NULL;
    const uint16_t xrmPort = 9763;

    try {
        setlogmask(LOG_UPTO(LOG_DEBUG));
        openlog("xrmd", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
        syslog(LOG_NOTICE, "Starting XRM Daemon:");
        syslog(LOG_NOTICE, "    XRM Version = %s", XRM_VERSION_STRING);
        syslog(LOG_NOTICE, "    Git Branch = %s", XRM_GIT_BRANCH);
        syslog(LOG_NOTICE, "    Git Commit = %s", XRM_GIT_COMMIT_HASH);

        // Create the system object
        sys = new xrm::system;
        sys->initLock();
        if (!sys->restore()) sys->initSystem();

        // Create the commands
        registry = new xrm::commandRegistry;
        registry->registerAll(*sys);

        // Accept connections and process commands
        ioService = new boost::asio::io_service;
        serv = new xrm::server(*ioService, xrmPort);
        serv->setSystem(sys);
        serv->setRegistry(registry);

        ioService->run();
    } catch (std::exception& e) {
        syslog(LOG_NOTICE, "Exception: %s", e.what());
    }

    if (serv != NULL) delete (serv);
    if (ioService != NULL) delete (ioService);
    if (registry != NULL) delete (registry);
    if (sys != NULL) delete (sys);

    return 0;
}
