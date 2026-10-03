#include <catch2/catch_session.hpp>
#include <kano_unattended.hpp>

int main(int argc, char** argv) {
    kano::infra::ConfigureUnattendedExecution();
    return Catch::Session().run(argc, argv);
}
