#include <iostream>
#include <vector>
#include <utility>
#include <memory>
#include <unordered_map>
#include <functional>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>

using namespace std::string_literals;
using namespace std::string_view_literals;

class CommandProcessor
{
public:
    virtual ~CommandProcessor() = default;
    virtual std::string Execute(std::string_view) = 0;
}; // end class CommandProcessor

class CommandExit : public CommandProcessor
{
public:
    std::string Execute(std::string_view input) final
    {
        std::size_t value = 0;

        input = input.substr(input.find_first_not_of(" \t\n"));
        input = input.substr(0, input.find_first_of(" \t\n"));

        try
        {
            if (!input.empty())
                value = static_cast<std::size_t>(std::stoull(std::string{input}));
        }
        catch (...)
        {
            value = 1;
        }

        std::exit(value);
        return {};
    }
}; // end class CommandExit

class CommandEcho : public CommandProcessor
{
public:
    std::string Execute(std::string_view input) final
    {
        input = input.substr(input.find_first_not_of(" \n\t"));
        if (!input.empty() && input[0] == '\'')
            input = input.substr(1, input.find_first_of('\'', 1) - 1);
        return std::string{input};
    }
}; // end class CommandEcho

template <typename T>
concept IsHasFind = requires(T obj, std::string_view req) {
    { obj.find(req) } -> std::convertible_to<typename T::iterator>;
};

template <IsHasFind Map>
class CommandType : public CommandProcessor
{
public:
    CommandType(const Map &map) : _commandMap(map) {}
    std::string Execute(std::string_view input) final
    {
        input = input.substr(input.find_first_not_of(" \n\t"));
        if (!input.empty() && input[0] == '\'')
            input = input.substr(1, input.find_first_of('\'', 1) - 1);
        else
            input = input.substr(0, input.find_first_of(" \n\t"));
            
        if (auto it = _commandMap.find(input); it != _commandMap.end())
        {
            return std::string(input) += " is a shell builtin"s;
        }
        else
        {
            const char *pathEnv = std::getenv("PATH");
            if (!pathEnv || input.empty())
                return std::string{input} + ": not found"s;

            std::istringstream pathStream(pathEnv);
            std::string directory;
            while (std::getline(pathStream, directory, ':'))
            {
                std::filesystem::path filePath = std::filesystem::path(directory) / input;
                if (std::filesystem::exists(filePath) && std::filesystem::is_regular_file(filePath))
                {
                    return std::string{input} += " is "s += filePath.string();
                }
            }
        }

        return std::string(input) += ": not found"s;
    }

private:
    const Map &_commandMap;
}; // end class CommandType

class CommandPwd : public CommandProcessor
{
public:
    std::string Execute(std::string_view) final
    {
        char buffer[PATH_MAX];
        if (getcwd(buffer, sizeof(buffer)) != nullptr)
        {
            return std::string(buffer);
        }

        return "Error: Unable to retrieve current working directory."s;
    }
}; // end class CommandPwd

class CommandCd : public CommandProcessor
{
public:
    std::string Execute(std::string_view input) final
    {
        input = input.substr(input.find_first_not_of(" \t\n"));
        input = input.substr(0, input.find_last_not_of(" \t\n") + 1);

        if (input.empty())
        {
            const char* homeDir = std::getenv("HOME");
            if (homeDir && chdir(homeDir) == 0)
            {
                return ""s;
            }
            return "Error: Unable to change to home directory."s;
        }

        std::string path{input};
        if (path.starts_with("~"))
        {
            const char* homeDir = std::getenv("HOME");
            if (!homeDir)
            {
                return "Error: HOME environment variable is not set."s;
            }

            path.replace(0, 1, homeDir);
        }

        if (chdir(path.c_str()) == 0)
        {
            return ""s;
        }

        return "cd: "s += path += ": No such file or directory"s;
    }
}; // end class CommandCd

class Processor
{
    using CommandMap = std::unordered_map<std::string_view, std::unique_ptr<CommandProcessor>>;

public:
    Processor()
    {
        _commandMap.emplace("exit"sv, std::make_unique<CommandExit>());
        _commandMap.emplace("echo"sv, std::make_unique<CommandEcho>());
        _commandMap.emplace("type"sv, std::make_unique<CommandType<CommandMap>>(_commandMap));
        _commandMap.emplace("pwd"sv, std::make_unique<CommandPwd>());
        _commandMap.emplace("cd"sv, std::make_unique<CommandCd>());
    }

    std::string Execute(std::string_view input)
    {
        auto wordStart = input.find_first_not_of(" \n\t");
        if (wordStart == std::string_view::npos)
            return {};
        auto wordEnd = input.find_first_of(" \n\t", wordStart);
        auto command = input.substr(wordStart, wordEnd);
        input = input.substr(wordEnd == std::string_view::npos ? command.size() : wordEnd);

        auto it = _commandMap.find(command);
        if (it == _commandMap.end())
        {
            return ExecuteExternal(command, input);
        }

        return it->second->Execute(input);
    }

private:
    std::string ExecuteExternal(std::string_view command, std::string_view input)
    {
        if (!input.empty())
            input = input.substr(input.find_first_not_of(" \n\t"));

        auto spacePos = input.find_first_of(" \n\t");

        const char *pathEnv = std::getenv("PATH");
        if (!pathEnv)
            return std::string{command} + ": command not found";

        std::vector<std::string> directories;
        std::string path{pathEnv};
        size_t pos = 0;
        while ((pos = path.find(':')) != std::string::npos)
        {
            directories.emplace_back(path.substr(0, pos));
            path.erase(0, pos + 1);
        }
        directories.emplace_back(path);

        for (const auto &dir : directories)
        {
            std::filesystem::path executable = std::filesystem::path(dir) / command;
            if (std::filesystem::exists(executable) && std::filesystem::is_regular_file(executable))
            {
                return ExecuteProgram(executable, input);
            }
        }

        return std::string{command} += ": command not found";
    }

    class ArgvHolder
    {
    public:
        void LoadArgv(std::string_view args)
        {
            size_t pos = 0;
            while ((pos = args.find(' ')) != std::string::npos)
            {
                char *str = new char[pos + 1];
                auto word = args.substr(0, pos);
                std::memcpy(str, word.data(), word.size());
                str[word.size()] = '\0';
                _argv.push_back(str);
                args = args.substr(args.find_first_not_of(" \n\t", pos));
            }
            char *str = new char[args.size() + 1];
            std::memcpy(str, args.data(), args.size());
            str[args.size()] = '\0';
            _argv.push_back(str);
            _argv.push_back(nullptr);
        }
        char *const *data()
        {
            return const_cast<char *const *>(_argv.data());
        }
        void PushWord(std::string_view iStr)
        {
            char *str = new char[iStr.size() + 1];
            std::memcpy(str, iStr.data(), iStr.size());
            str[iStr.size()] = '\0';
            _argv.push_back(str);
        }
        ~ArgvHolder()
        {
            for (auto &p : _argv)
            {
                delete[] (p);
            }
        }

        const char *&operator[](size_t val)
        {
            return _argv[val];
        }

    private:
        std::vector<const char *> _argv;
    }; // end calss ArgvHolder

    std::string ExecuteProgram(const std::filesystem::path &executable, std::string_view args)
    {
        ArgvHolder argv;
        argv.PushWord(executable.string());
        argv.LoadArgv(args);

        if (fork() == 0)
        {
            execvp(argv[0], const_cast<char *const *>(argv.data()));
            _exit(EXIT_FAILURE);
        }

        int status;
        wait(&status);

        return WIFEXITED(status) ? "" : "Error: Failed to execute external command!";
    }

    CommandMap _commandMap;
}; // end class Processor

int main()
{
    // Flush after every std::cout / std:cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    Processor processor;

    std::cout << "$ ";
    std::string input;
    while (std::getline(std::cin, input))
    {
        auto out = processor.Execute(input);
        if (!out.empty())
            std::cout << out << std::endl;

        std::cout << "$ ";
    }
}
