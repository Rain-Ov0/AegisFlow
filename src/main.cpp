#include "aegisflow/app/handler.hpp"
#include "aegisflow/app/process_signal_waiter.hpp"
#include "aegisflow/config/config.hpp"
#include "aegisflow/log/logger.hpp"
#include "aegisflow/timer/timer.hpp"

#include <exception>
#include <iostream>

namespace {

bool startRuntime(
    const aegisflow::config::AppConfig& config,
    aegisflow::log::Logger& logger,
    aegisflow::timer::Timer& timer,
    aegisflow::app::Handler& handler
) {
    if (logger.init(config.logger) != aegisflow::log::LoggerStatus::Ok ||
        logger.start() != aegisflow::log::LoggerStatus::Ok) {
        std::cerr << "start Logger failed\n";
        logger.stop();
        (void)logger.join();
        return false;
    }
    AEGISFLOW_LOG_INFO("Logger started");

    if (timer.init(config.timer) != aegisflow::timer::TimerStatus::Ok ||
        timer.start() != aegisflow::timer::TimerStatus::Ok) {
        AEGISFLOW_LOG_ERROR("start Timer failed");
        timer.stop();
        (void)timer.join();
        logger.stop();
        (void)logger.join();
        return false;
    }
    AEGISFLOW_LOG_INFO("Timer started");

    if (handler.init(config.handler) != aegisflow::app::HandlerStatus::Ok ||
        handler.start() != aegisflow::app::HandlerStatus::Ok) {
        AEGISFLOW_LOG_ERROR("start Handler failed");
        handler.stop();
        (void)handler.join();
        timer.stop();
        (void)timer.join();
        logger.stop();
        (void)logger.join();
        return false;
    }
    AEGISFLOW_LOG_INFO("Handler started");
    return true;
}

bool stopRuntime(
    aegisflow::log::Logger& logger,
    aegisflow::timer::Timer& timer,
    aegisflow::app::Handler& handler
) noexcept {
    handler.stop();
    const bool handler_ok =
        handler.join() == aegisflow::app::HandlerStatus::Ok;
    if (handler_ok) {
        AEGISFLOW_LOG_INFO("Handler stopped");
    } else {
        AEGISFLOW_LOG_ERROR("stop Handler failed");
    }

    timer.stop();
    const bool timer_ok = timer.join() == aegisflow::timer::TimerStatus::Ok;
    if (timer_ok) {
        AEGISFLOW_LOG_INFO("Timer stopped");
    } else {
        AEGISFLOW_LOG_ERROR("stop Timer failed");
    }

    logger.stop();
    const bool logger_ok =
        logger.join() == aegisflow::log::LoggerStatus::Ok;
    if (!logger_ok) {
        std::cerr << "stop Logger failed\n";
    }
    return handler_ok && timer_ok && logger_ok;
}

}  // 命名空间

int main(const int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: AegisFlow <config_file>\n";
        return 1;
    }

    try {
        const auto config = aegisflow::config::loadAppConfig(argv[1]);
        auto& logger = aegisflow::log::Logger::instance();
        auto& timer = aegisflow::timer::Timer::instance();
        auto& handler = aegisflow::app::Handler::instance();
        aegisflow::app::ProcessSignalWaiter signal_waiter;
        if (!signal_waiter.blockTerminationSignals()) {
            std::cerr << "block termination signals failed\n";
            return 1;
        }
        if (!startRuntime(config, logger, timer, handler)) {
            return 1;
        }

        int exit_code = 0;
        const auto signal_result = signal_waiter.wait();
        if (!signal_result.ok) {
            AEGISFLOW_LOG_ERROR("wait termination signal failed");
            exit_code = 1;
        }
        if (handler.state() == aegisflow::app::HandlerState::Failed) {
            AEGISFLOW_LOG_ERROR("Handler entered failed state");
            exit_code = 1;
        }
        if (!stopRuntime(logger, timer, handler)) {
            std::cerr << "stop runtime failed\n";
            exit_code = 1;
        }
        return exit_code;
    } catch (const std::exception& error) {
        std::cerr << "configure AegisFlow failed: " << error.what() << '\n';
        return 1;
    }
}
