#include <stdint.h>
#include <iostream>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <dmtr/libos/persephone.hh>
#include <dmtr/annot.h>
#include <dmtr/libos/io_queue.hh>
#include <dmtr/libos/memory_queue.hh>
#include "lwip_queue.hh"

int PspServiceUnit::init(int argc, char *argv[]) {


    if (my_type == dmtr::io_queue::category_id::NETWORK_Q) { //maybe prefix
        DMTR_OK(dmtr::lwip_queue::init_dpdk(argc, argv));
    }

    if (argc > 0) {
        std::string log_directory;
        namespace po = boost::program_options;
        po::options_description desc{"IO queue API options"};
        desc.add_options()
            ("log-dir,L", po::value<std::string>(&log_directory)->default_value("./"), "Log directory");
        po::variables_map vm;
        po::parsed_options parsed =
            po::command_line_parser(argc, argv).options(desc).allow_unregistered().run();
        po::store(parsed, vm);
        po::notify(vm);
        dmtr_log_directory = log_directory;
    }

    ioqapi.register_queue_ctor(dmtr::io_queue::MEMORY_Q, dmtr::memory_queue::new_object);
    ioqapi.register_queue_ctor(dmtr::io_queue::NETWORK_Q, dmtr::lwip_queue::new_object);
    ioqapi.register_queue_ctor(dmtr::io_queue::SHARED_Q, dmtr::shared_queue::new_object);

    return 0;
}
