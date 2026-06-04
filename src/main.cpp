#include <iostream>
#include <exception>
#include "aegisflow/log/logger.hpp"
#include "aegisflow/config/config.hpp"
#include "aegisflow/net/http_server.hpp"
#include "aegisflow/app/risk_service.hpp"

#include "event.pb.h"
#include "decision.pb.h"

// 服务器端启动
/*
1. 加载配置文件
2. 初始化日志
3. 初始化风险服务
4. 启动HTTP服务器
*/
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: AegisFlow <config_file>" << std::endl;
        return 1;
    }
    
    std::cout << "Hello, AegisFlow!" << std::endl;
    aegisflow::config::Config config_;
    if (!config_.loadFromFile(argv[1])) {
        std::cerr << "load config failed" << std::endl;
        return 1;
    }

    aegisflow::log::Logger:: instance().init(
        aegisflow::log::Logger:: instance().stringToLogLevel(config_.getString("log.level", "INFO")),
        config_.getString("log.file", "logs/log.log")
    );

    aegisflow::app::RiskService risk_service_;
    aegisflow::net::HttpServer http_server(
        config_.getString("http_server.host", "0.0.0.0"),
        config_.getInt("http_server.port", 8080),
        config_.getInt("http_server.io_threads", 1),
        config_.getUInt64("http_server.max_body_size", 1024 * 1024),
        risk_service_
    );

    http_server.run();

    return 0;
}