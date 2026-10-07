#include <roc/vehicle/config.hpp>
#include <roc/vehicle/crypto.hpp>
#include <roc/vehicle/state_store.hpp>
#include <roc/vehicle/task_http_client.hpp>
#include <roc/vehicle/version.hpp>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root{argv[1]};
    const auto hash = roc::vehicle::sha256_hex("");
    if (!hash || hash.value() !=
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") return 3;
    auto store = roc::vehicle::DurableStateStore::open(root / "state.sqlite3");
    if (!store) return 4;
    roc::vehicle::ClientConfig config;
    config.base_url = "http://127.0.0.1:1"; // Synthetic; no requests are made.
    config.state_directory = root;
    config.artifact_directory = root;
    auto client = roc::vehicle::DeviceTaskHttpClient::create(config, [] {
        return roc::vehicle::Result<std::string>{"synthetic-consumer-token"};
    });
    if (!client) return 5;
    if (roc::vehicle::library_version() != "0.2.1-rc2") return 6;
    if (roc::vehicle::protocol_compatibility_version() !=
        "roc-system-v0.2.1@a9c601d6977d89000438854a24e3199573004a2f") return 7;
    std::cout << "installed_consumer cxx17=1 crypto_sqlite_http_linked=1 no_network=1 version="
              << roc::vehicle::library_version() << '\n';
    return 0;
}
