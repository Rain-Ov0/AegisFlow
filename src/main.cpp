#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

#include "aegisflow/app/risk_service.hpp"
#include "aegisflow/config/config.hpp"
#include "aegisflow/log/logger.hpp"
#include "aegisflow/net/http_server.hpp"
#include "aegisflow/risk/blacklist_manager.hpp"
#include "aegisflow/storage/mysql_dao.hpp"

#include "decision.pb.h"
#include "event.pb.h"

namespace {

uint16_t readPort(
    const aegisflow::config::Config& config,
    const std::string& key,
    uint16_t default_value
) {
    const int value = config.getInt(key, default_value);
    if (value <= 0 || value > 65535) {
        return default_value;
    }

    return static_cast<uint16_t>(value);
}

unsigned int readUnsignedInt(
    const aegisflow::config::Config& config,
    const std::string& key,
    unsigned int default_value
) {
    const int value = config.getInt(key, static_cast<int>(default_value));
    if (value < 0) {
        return default_value;
    }

    return static_cast<unsigned int>(value);
}

aegisflow::storage::MysqlConfig buildMysqlConfig(
    const aegisflow::config::Config& config
) {
    aegisflow::storage::MysqlConfig mysql_config;

    mysql_config.host = config.getString("mysql.host", mysql_config.host);
    mysql_config.port = readPort(config, "mysql.port", mysql_config.port);
    mysql_config.user = config.getString("mysql.user", mysql_config.user);
    mysql_config.password = config.getString(
        "mysql.password",
        mysql_config.password
    );
    mysql_config.database = config.getString(
        "mysql.database",
        mysql_config.database
    );
    mysql_config.charset = config.getString("mysql.charset", mysql_config.charset);
    mysql_config.connect_timeout_sec = readUnsignedInt(
        config,
        "mysql.connect_timeout_sec",
        mysql_config.connect_timeout_sec
    );
    mysql_config.read_timeout_sec = readUnsignedInt(
        config,
        "mysql.read_timeout_sec",
        mysql_config.read_timeout_sec
    );
    mysql_config.write_timeout_sec = readUnsignedInt(
        config,
        "mysql.write_timeout_sec",
        mysql_config.write_timeout_sec
    );

    return mysql_config;
}

aegisflow::risk::BlacklistManagerOptions buildBlacklistOptions(
    const aegisflow::config::Config& config
) {
    aegisflow::risk::BlacklistManagerOptions options;

    options.bloom_bits = static_cast<size_t>(
        config.getUInt64("blacklist.bloom_bits", options.bloom_bits)
    );
    options.bloom_hashes = static_cast<size_t>(
        config.getUInt64("blacklist.bloom_hashes", options.bloom_hashes)
    );
    options.cache_capacity = static_cast<size_t>(
        config.getUInt64("blacklist.cache_capacity", options.cache_capacity)
    );
    options.negative_ttl_ms = config.getUInt64(
        "blacklist.negative_ttl_ms",
        options.negative_ttl_ms
    );
    options.positive_ttl_ms = config.getUInt64(
        "blacklist.positive_ttl_ms",
        options.positive_ttl_ms
    );

    return options;
}

} // namespace

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

        aegisflow::storage::MysqlDao mysql_dao(buildMysqlConfig(config_));
        aegisflow::risk::BlacklistManager blacklist_manager(
            &mysql_dao,
            buildBlacklistOptions(config_)
        );

        if (mysql_dao.connect()) {
            if (blacklist_manager.loadFromMysql()) {
                LOG_INFO(
                    "loaded blacklist entries: " +
                    std::to_string(blacklist_manager.localSize())
                );
            } else {
                LOG_WARN(
                    "load blacklist from mysql failed: " +
                    mysql_dao.lastError()
                );
            }
        } else {
            LOG_WARN("mysql connect failed: " + mysql_dao.lastError());
        }

        aegisflow::app::RiskService risk_service_(
            config_.getInt("worker_pool.threads", 0),
            rule_file,
            &blacklist_manager
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