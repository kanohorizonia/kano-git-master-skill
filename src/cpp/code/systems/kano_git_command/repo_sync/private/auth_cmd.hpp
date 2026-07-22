#pragma once

#include <functional>
#include <memory>
#include <string>

namespace kano::git::commands {

struct AuthCommandOptions {
    struct Doctor {
        std::string repo{"."};
        std::string remote;
        std::string url;
        bool selectedRemotes{false};
        bool allLocalRemotes{false};
        bool noRecursive{false};
        bool noCache{false};
        bool refreshCache{false};
        bool fix{false};
    } doctor;

    struct Test {
        std::string repo{"."};
        std::string remote;
        std::string url;
        bool selectedRemotes{false};
        bool allLocalRemotes{false};
        bool noRecursive{false};
        bool noCache{false};
        bool refreshCache{false};
    } test;
};

auto MakeAuthDoctorCommandCallback(const std::shared_ptr<AuthCommandOptions>& InOptions)
    -> std::function<void()>;
auto MakeAuthTestCommandCallback(const std::shared_ptr<AuthCommandOptions>& InOptions)
    -> std::function<void()>;

} // namespace kano::git::commands
