#include <iostream>
#include "logger.hpp"
#include "config.hpp"

#include "event.pb.h"
#include "decision.pb.h"

int main() {
    std::cout << "Hello, AegisFlow!" << std::endl;
    Config config;
    config.loadFromFile("../config/server.conf");
    std::cout << "Server host: " << config.getString("server.host") << std::endl;
    std::cout << "Server port: " << config.getInt("server.port") << std::endl;
    std::cout << "Log level: " << config.getString("log.level") << std::endl;
    std::cout << "Log file: " << config.getString("log.file") << std::endl;
    return 0;
}