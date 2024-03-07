#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

/**
 * Abstract interface for defining console modules, which are handlers for
 * a group of commands. For instance, the group of commands starting with "os."
 * will all be handled by a single ConsoleModule.
 */
class ConsoleModule {
public:
    virtual ~ConsoleModule() = default;

    /**
     * Perform the action specified by the given command
     * @param command Command to run (excludes the module name prefix)
     * @return User-facing reply string. boost::none is considered failure to handle the command.
     * @todo Change the parameter to std::string_view type
     */
    virtual std::optional<std::string> HandleCommand(const std::string& command) = 0;
};

/**
 * Interface to the console.
 */
class Console {
    std::map<std::string /*module_name*/, std::unique_ptr<ConsoleModule>> modules;

public:
    virtual ~Console() = default;

    /**
     * Perform the action specified by the given command
     * @param command Command to run
     * @return User-facing reply string. boost::none is considered failure to handle the command.
     */
    std::optional<std::string> HandleCommand(const std::string& command);

    /**
     * Register the given module using the given category name.
     */
    void RegisterModule(const std::string& name, std::unique_ptr<ConsoleModule> module);
};

/**
 * Console implementation that waits for incoming connections on a given port
 * and consecutively reads console commands from any clients.
 */
class NetworkConsole : public Console {
    std::unique_ptr<class NetworkConsoleServer> server;

public:
    NetworkConsole(unsigned port);
    ~NetworkConsole();

    // Blocks until Stop() is called
    void Run();

    // Must be called before object destruction
    void Stop();
};
