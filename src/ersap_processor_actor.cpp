#include <et.h>
#include <iostream>
#include <cstring>
#include <csignal>
#include <atomic>
#include <cstdlib>
#include <string>

std::atomic<bool> keep_running{true};

void signalHandler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\nReceived interrupt signal, shutting down..." << std::endl;
        keep_running = false;
    }
}

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " [options]\n"
              << "Options:\n"
              << "  -f <ET_file>      ET system file name (required)\n"
              << "  -h <host>         ET host (default: localhost)\n"
              << "  -p <port>         ET port (default: 11111)\n"
              << "  -s <station>      Station name (default: ERSAP_PROCESSOR)\n"
              << "  --help            Show this help message\n";
}

int main(int argc, char* argv[]) {
    std::string et_filename;
    std::string et_host = "localhost";
    int et_port = ET_SERVER_PORT;
    std::string station_name = "ERSAP_PROCESSOR";

    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            et_filename = argv[++i];
        } else if (std::strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            et_host = argv[++i];
        } else if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            et_port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            station_name = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (et_filename.empty()) {
        std::cerr << "Error: ET system file name is required\n\n";
        printUsage(argv[0]);
        return 1;
    }

    // Install signal handler for graceful shutdown
    std::signal(SIGINT, signalHandler);

    et_sys_id et_sys;
    et_att_id et_att;
    et_stat_id et_stat;
    et_openconfig openconfig;
    et_statconfig statconfig;
    int status;

    // Initialize ET open configuration
    status = et_open_config_init(&openconfig);
    if (status != ET_OK) {
        std::cerr << "Error: Failed to initialize ET open config\n";
        return 1;
    }

    et_open_config_setwait(openconfig, ET_OPEN_WAIT);
    et_open_config_sethost(openconfig, et_host.c_str());
    et_open_config_setcast(openconfig, ET_DIRECT);
    et_open_config_setserverport(openconfig, et_port);

    struct timespec timeout;
    timeout.tv_sec = 10;
    timeout.tv_nsec = 0;
    et_open_config_settimeout(openconfig, timeout);

    // Open ET system
    std::cout << "Opening ET system: " << et_filename << " at " << et_host << ":" << et_port << std::endl;
    status = et_open(&et_sys, et_filename.c_str(), openconfig);
    et_open_config_destroy(openconfig);

    if (status != ET_OK) {
        std::cerr << "Error: Failed to open ET system (status " << status << ")\n";
        return 1;
    }

    std::cout << "ET system opened successfully\n";

    // Initialize station configuration
    status = et_station_config_init(&statconfig);
    if (status != ET_OK) {
        std::cerr << "Error: Failed to initialize station config\n";
        et_close(et_sys);
        return 1;
    }

    // Configure station (single consumer, blocking, select all events)
    et_station_config_setuser(statconfig, ET_STATION_USER_MULTI);
    et_station_config_setrestore(statconfig, ET_STATION_RESTORE_OUT);
    et_station_config_setprescale(statconfig, 1);
    et_station_config_setcue(statconfig, 10);
    et_station_config_setselect(statconfig, ET_STATION_SELECT_ALL);
    et_station_config_setblock(statconfig, ET_STATION_BLOCKING);

    // Create station or attach if it already exists
    std::cout << "Creating/attaching to station: " << station_name << std::endl;
    status = et_station_create_at(et_sys, &et_stat, station_name.c_str(),
                                   statconfig, ET_END, 0);

    if (status == ET_ERROR_EXISTS) {
        // Station already exists, get its ID
        status = et_station_name_to_id(et_sys, &et_stat, station_name.c_str());
        if (status != ET_OK) {
            std::cerr << "Error: Station exists but cannot get ID (status " << status << ")\n";
            et_station_config_destroy(statconfig);
            et_close(et_sys);
            return 1;
        }
        std::cout << "Station already exists, using existing station\n";
    } else if (status != ET_OK) {
        std::cerr << "Error: Failed to create station (status " << status << ")\n";
        et_station_config_destroy(statconfig);
        et_close(et_sys);
        return 1;
    } else {
        std::cout << "Station created successfully\n";
    }

    et_station_config_destroy(statconfig);

    // Attach to the station
    status = et_station_attach(et_sys, et_stat, &et_att);
    if (status != ET_OK) {
        std::cerr << "Error: Failed to attach to station (status " << status << ")\n";
        et_close(et_sys);
        return 1;
    }

    std::cout << "Attached to station successfully\n";
    std::cout << "Starting event processing loop...\n\n";

    const size_t EXPECTED_SIZE = 16 * sizeof(double);
    et_event* pe;

    // Main event processing loop
    while (keep_running) {
        // Get event from ET system (blocking)
        status = et_event_get(et_sys, et_att, &pe, ET_SLEEP, NULL);

        if (status == ET_ERROR_DEAD) {
            std::cerr << "Error: ET system is dead\n";
            break;
        } else if (status == ET_ERROR_WAKEUP) {
            std::cout << "Woken up, exiting...\n";
            break;
        } else if (status != ET_OK) {
            std::cerr << "Error: Failed to get event (status " << status << ")\n";
            break;
        }

        // Get event data and length
        void* data_ptr;
        size_t data_len;
        et_event_getdata(pe, &data_ptr);
        et_event_getlength(pe, &data_len);

        // Validate payload size
        if (data_len < EXPECTED_SIZE) {
            std::cerr << "Error during the data transport: received " << data_len
                      << " bytes, expected minimum " << EXPECTED_SIZE << " bytes\n";

            // Return event and continue
            status = et_event_put(et_sys, et_att, pe);
            if (status != ET_OK) {
                std::cerr << "Error: Failed to put event back (status " << status << ")\n";
                break;
            }
            continue;
        }

        // Interpret data as array of doubles
        double* doubles = static_cast<double*>(data_ptr);

        // Print four-vectors
        std::cout << "π+ four-vector: E=" << doubles[0]
                  << ", Px=" << doubles[1]
                  << ", Py=" << doubles[2]
                  << ", Pz=" << doubles[3] << "\n";

        std::cout << "π- four-vector: E=" << doubles[4]
                  << ", Px=" << doubles[5]
                  << ", Py=" << doubles[6]
                  << ", Pz=" << doubles[7] << "\n";

        std::cout << "γ1 four-vector: E=" << doubles[8]
                  << ", Px=" << doubles[9]
                  << ", Py=" << doubles[10]
                  << ", Pz=" << doubles[11] << "\n";

        std::cout << "γ2 four-vector: E=" << doubles[12]
                  << ", Px=" << doubles[13]
                  << ", Py=" << doubles[14]
                  << ", Pz=" << doubles[15] << "\n\n";

        // Return event to ET system
        status = et_event_put(et_sys, et_att, pe);
        if (status == ET_ERROR_DEAD) {
            std::cerr << "Error: ET system is dead\n";
            break;
        } else if (status != ET_OK) {
            std::cerr << "Error: Failed to put event (status " << status << ")\n";
            break;
        }
    }

    // Clean shutdown
    std::cout << "\nCleaning up...\n";

    status = et_station_detach(et_sys, et_att);
    if (status != ET_OK) {
        std::cerr << "Warning: Failed to detach from station (status " << status << ")\n";
    }

    status = et_close(et_sys);
    if (status != ET_OK) {
        std::cerr << "Warning: Failed to close ET system (status " << status << ")\n";
    }

    std::cout << "Shutdown complete\n";
    return 0;
}
