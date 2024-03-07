#include "console.hpp"
#include "utility/simple_tcp.hpp"

#include <boost/asio.hpp>

#include <range/v3/algorithm/find.hpp>

#include <iostream>
#include <optional>

std::optional<std::string> Console::HandleCommand(const std::string& command) {
    const char* syntax_error_reply = "Syntax: <module>.<command> [<parameters>]";

    auto dot_it = ranges::find(command, '.');
    if (dot_it == command.end())
        return {{syntax_error_reply}};

    std::string desired_module_name{command.begin(), dot_it};

    // Find a module matching the prefix
    for (auto& module : modules) {
        if (module.first != desired_module_name)
            continue;

        std::string sub_command{dot_it + 1, command.end()};
        // TODO: command may include a newline character, so this check is pretty much useless currently
        if (sub_command.length() == 0)
            return {{syntax_error_reply}};

        return module.second->HandleCommand(sub_command);
    }

    return "No module called \"" + desired_module_name + "\" registered";
}

void Console::RegisterModule(const std::string& name, std::unique_ptr<ConsoleModule> module) {
    if (modules.count(name))
        throw std::runtime_error("Module \"" + name + "\" already registered");

    modules[name] = std::move(module);
}

class NetworkConsoleServer : public SimpleTCPServer {
    Console& console;

    boost::asio::streambuf streambuf {};

    std::optional<boost::asio::ip::tcp::socket> client;

    void OnClientConnected(boost::asio::ip::tcp::socket socket) override {
        client = std::move(socket);
        SetupLineReceivedHandler();
    }

    void SetupLineReceivedHandler() {
        auto handler = [&](boost::system::error_code ec, std::size_t /*bytes_received*/) {
            if (!ec) {
                std::istream stream(&streambuf);
                std::string command;
                std::getline(stream, command);

                auto reply = console.HandleCommand(command);
                boost::asio::write(*client, boost::asio::buffer(*reply, reply->size()));
            }

            SetupLineReceivedHandler();
        };
        boost::asio::async_read_until(*client, streambuf, '\n', handler);
    }

public:
    NetworkConsoleServer(Console& console, uint16_t port) : SimpleTCPServer(port), console(console) {}
};

NetworkConsole::NetworkConsole(unsigned port) : server(std::make_unique<NetworkConsoleServer>(*this, port)) {
}

NetworkConsole::~NetworkConsole() = default;

void NetworkConsole::Run() {
    server->RunTCPServer();
}

void NetworkConsole::Stop() {
    server->StopTCPServer();
}
