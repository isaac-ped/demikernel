#include "common.hh"
#include <iostream>

namespace bpo = boost::program_options;

int parse_args(int argc, char **argv, bpo::options_description &opts, CommonOptions &common) {
    opts.add_options()
                ("help", "produce help message")
                ("log-dir,L",
                    bpo::value<std::string>(&common.log_dir)->default_value("."),
                    "Log directory")
                ("config-path,c",
                    bpo::value<std::string>(&common.config_path)->default_value("./config.yaml"),
                    "lwip config path");

    bpo::variables_map vm;
    try {
        bpo::parsed_options parsed =
            bpo::command_line_parser(argc, argv).options(opts).run();
        bpo::store(parsed, vm);
        if (vm.count("help")) {
            std::cout << opts << std::endl;
            return 1;
        }
        bpo::notify(vm);
    } catch (const bpo::error &e) {
        std::cerr << e.what() << std::endl;
        std::cerr << opts << std::endl;
        return 1;
    }
    return 0;
}

int pin_thread(pthread_t thread, int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    int rtn = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    if (rtn != 0) {
        fprintf(stderr, "could not pin thread: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
