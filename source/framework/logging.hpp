#pragma once

#include <spdlog/spdlog.h>

#include <utility>

class LogManager {
    spdlog::sink_ptr sink;
    std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> loggers;

public:
    LogManager(spdlog::sink_ptr sink_) : sink(sink_) {
    }

    void ChangeSink(spdlog::sink_ptr new_sink) {
        sink = new_sink;
    }

    std::shared_ptr<spdlog::logger> GetLogger(const std::string& name) {
        return loggers.at(name);
    }

    std::shared_ptr<spdlog::logger> RegisterLogger(std::string name) {
        auto logger = std::make_shared<spdlog::logger>(name, sink);
        logger->set_pattern("[%n] [%l] %v");
        auto ret = loggers.emplace(std::move(name), std::move(logger));
        return ret.first->second;
    }
};
