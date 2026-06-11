#include <exception>
#include <iostream>
#include <string>

#include "aegisflow/app/risk_service.hpp"
#include "aegisflow/config/config.hpp"
#include "aegisflow/log/logger.hpp"
#include "aegisflow/net/http_server.hpp"

#include "event.pb.h"
#include "decision.pb.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: AegisFlow <config_file>" << std::endl;
        return 1;
    }

    aegisflow::config::Config config_;
    if (!config_.loadFromFile(argv[1])) {
        std::cerr << "load config failed" << std::endl;
        return 1;
    }

    aegisflow::log::Logger::instance().init(
        aegisflow::log::Logger::instance().stringToLogLevel(
            config_.getString("log.level", "INFO")
        ),
        config_.getString("log.file", "logs/log.log")
    );

    try {
        const std::string rule_file =
            config_.getString("rule.file", "config/rules.dsl");

        aegisflow::app::RiskService risk_service_(
            config_.getInt("worker_pool.threads", 0),
            rule_file
        );

        aegisflow::net::HttpServer http_server(
            config_.getString(
                "http_server.host",
                config_.getString("server.host", "0.0.0.0")
            ),
            config_.getInt(
                "http_server.port",
                config_.getInt("server.port", 8080)
            ),
            config_.getInt(
                "http_server.io_threads",
                config_.getInt("server.io_threads", 1)
            ),
            config_.getUInt64(
                "http_server.max_body_size",
                config_.getUInt64("server.max_body_size", 1024 * 1024)
            ),
            risk_service_
        );

        http_server.run();
    } catch (const std::exception& e) {
        std::cerr << "start AegisFlow failed: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}