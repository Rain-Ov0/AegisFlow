#pragma once

#include "aegisflow/app/handler.hpp"
#include "aegisflow/log/logger.hpp"
#include "aegisflow/timer/timer.hpp"

#include <string>

namespace aegisflow::config {

struct AppConfig {
    log::LoggerConfig logger;
    timer::TimerConfig timer;
    app::HandlerConfig handler;

    bool operator==(const AppConfig& other) const {
        return logger == other.logger &&
               timer == other.timer &&
               handler == other.handler;
    }
};

// 配置文件只在这里完成解析、默认值填充和跨字段容量校验。
// main 与业务组件只接收已经建立不变量的强类型配置。
[[nodiscard]] AppConfig loadAppConfig(const std::string& path);

}  // 命名空间 aegisflow::config
