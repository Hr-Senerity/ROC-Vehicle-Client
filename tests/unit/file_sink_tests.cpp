#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/file_sink.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

constexpr auto kTaskId = "123e4567-e89b-12d3-a456-426614174010";

roc::vehicle::ArtifactManifest manifest_for(
    roc::vehicle::ResourceType type,
    std::string_view content,
    std::string content_type) {
    const auto digest = roc::vehicle::sha256_hex(content);
    EXPECT(digest);
    return {kTaskId,
            "123e4567-e89b-12d3-a456-426614174011",
            "123e4567-e89b-12d3-a456-426614174012",
            type,
            "123e4567-e89b-12d3-a456-426614174013",
            "123e4567-e89b-12d3-a456-426614174014",
            std::move(content_type),
            content.size(),
            digest ? digest.value() : std::string(64, '0'),
            1,
            type == roc::vehicle::ResourceType::road_network ? 1 : 0,
            std::string{"/api/device/tasks/"} + kTaskId + "/artifact"};
}

roc::vehicle::ArtifactHttpMetadata metadata_for(
    const roc::vehicle::ArtifactManifest& manifest) {
    return {manifest.content_type + "; charset=binary", manifest.sha256,
            manifest.byte_size, manifest.byte_size};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

void test_map_atomic_commit(const std::filesystem::path& root, const std::string& media_type) {
    const std::string content = "artifact-data";
    const auto manifest = manifest_for(roc::vehicle::ResourceType::map, content,
                                       media_type);
    auto sink = roc::vehicle::FileSink::create(root, manifest);
    EXPECT(sink);
    if (!sink) {
        return;
    }
    EXPECT(sink.value()->write(
        reinterpret_cast<const std::uint8_t*>(content.data()), 4));
    EXPECT(sink.value()->write(
        reinterpret_cast<const std::uint8_t*>(content.data() + 4),
        content.size() - 4));
    EXPECT(std::filesystem::exists(sink.value()->part_path()));
    EXPECT(sink.value()->seal(metadata_for(manifest)));
    const auto delivered = sink.value()->commit();
    EXPECT(delivered);
    if (delivered) {
        EXPECT(read_file(delivered.value().path) == content);
        EXPECT(delivered.value().path.filename().string().find("map-v1-") == 0);
    }
    EXPECT(!std::filesystem::exists(sink.value()->part_path()));

    auto duplicate = roc::vehicle::FileSink::create(root, manifest);
    EXPECT(duplicate);
    if (duplicate) {
        EXPECT(duplicate.value()->write(
            reinterpret_cast<const std::uint8_t*>(content.data()), content.size()));
        EXPECT(duplicate.value()->seal(metadata_for(manifest)));
        EXPECT(duplicate.value()->commit());
        EXPECT(!std::filesystem::exists(duplicate.value()->part_path()));
    }
}

void test_hash_failure_cleans_part(const std::filesystem::path& root, const std::string& media_type) {
    const std::string expected = "artifact-data";
    const std::string corrupt = "artifact-datb";
    const auto manifest = manifest_for(roc::vehicle::ResourceType::map, expected,
                                       media_type);
    std::filesystem::path part;
    {
        auto sink = roc::vehicle::FileSink::create(root, manifest);
        EXPECT(sink);
        if (!sink) {
            return;
        }
        part = sink.value()->part_path();
        EXPECT(sink.value()->write(
            reinterpret_cast<const std::uint8_t*>(corrupt.data()), corrupt.size()));
        const auto sealed = sink.value()->seal(metadata_for(manifest));
        EXPECT(!sealed);
        if (!sealed) EXPECT(sealed.error().server_code == "artifact_hash_mismatch");
        EXPECT(!std::filesystem::exists(sink.value()->final_path()));
    }
    EXPECT(!std::filesystem::exists(part));
}

void test_http_metadata_failures(const std::filesystem::path& root, const std::string& media_type) {
    const std::string content = "artifact-data";
    const auto manifest = manifest_for(roc::vehicle::ResourceType::map, content,
                                       media_type);
    const auto run = [&](roc::vehicle::ArtifactHttpMetadata metadata,
                         const std::filesystem::path& directory, const char* expected_code) {
        auto sink = roc::vehicle::FileSink::create(directory, manifest);
        EXPECT(sink);
        if (!sink) {
            return;
        }
        EXPECT(sink.value()->write(
            reinterpret_cast<const std::uint8_t*>(content.data()), content.size()));
        const auto sealed = sink.value()->seal(metadata);
        EXPECT(!sealed);
        if (!sealed) EXPECT(sealed.error().server_code == expected_code);
    };
    auto wrong_type = metadata_for(manifest);
    wrong_type.content_type = media_type == "image/png" ? "image/jpeg" : "image/png";
    run(wrong_type, root / "type", "artifact_metadata_mismatch");
    auto wrong_header_hash = metadata_for(manifest);
    wrong_header_hash.sha256 = std::string(64, '0');
    run(wrong_header_hash, root / "header-hash", "artifact_hash_mismatch");
    auto wrong_length = metadata_for(manifest);
    wrong_length.content_length = manifest.byte_size + 1;
    run(wrong_length, root / "length", "artifact_metadata_mismatch");
}

void test_invalid_existing_file_is_replaced(const std::filesystem::path& root) {
    const std::string content = "artifact-data";
    const auto manifest = manifest_for(roc::vehicle::ResourceType::map, content,
                                       "image/png");
    auto sink = roc::vehicle::FileSink::create(root, manifest);
    EXPECT(sink);
    if (!sink) {
        return;
    }
    {
        std::ofstream output{sink.value()->final_path(), std::ios::binary};
        output << "artifact-datb";
    }
    EXPECT(sink.value()->write(
        reinterpret_cast<const std::uint8_t*>(content.data()), content.size()));
    EXPECT(sink.value()->seal(metadata_for(manifest)));
    const auto delivered = sink.value()->commit();
    EXPECT(delivered);
    if (delivered) {
        EXPECT(read_file(delivered.value().path) == content);
    }
}

void test_road_validation_and_commit(const std::filesystem::path& root) {
    const std::string document =
        R"JSON({"schema_version":1,"coordinate_mode":"metric","nodes":[{"id":"a","x":0,"y":0,"kind":"waypoint","label":"A"},{"id":"b","x":1,"y":1,"kind":"waypoint","label":"B"}],"edges":[{"id":"e","from":"a","to":"b","direction":"forward","max_speed_mps":1.5}]})JSON";
    const auto manifest = manifest_for(
        roc::vehicle::ResourceType::road_network, document,
        "application/vnd.roc.road-network+json");
    auto sink = roc::vehicle::FileSink::create(
        root, manifest, roc::vehicle::FileSinkOptions{"metric"});
    EXPECT(sink);
    if (sink) {
        EXPECT(sink.value()->write(
            reinterpret_cast<const std::uint8_t*>(document.data()), document.size()));
        EXPECT(sink.value()->seal(metadata_for(manifest)));
        const auto delivered = sink.value()->commit();
        EXPECT(delivered);
        if (delivered) {
            EXPECT(read_file(delivered.value().path) == document);
        }
    }

    auto mismatch = roc::vehicle::FileSink::create(
        root / "mismatch", manifest,
        roc::vehicle::FileSinkOptions{"legacy-normalized"});
    EXPECT(mismatch);
    if (mismatch) {
        EXPECT(mismatch.value()->write(
            reinterpret_cast<const std::uint8_t*>(document.data()), document.size()));
        const auto sealed = mismatch.value()->seal(metadata_for(manifest));
        EXPECT(!sealed);
        if (!sealed) {
            EXPECT(sealed.error().server_code == "coordinate_mode_mismatch");
        }
    }
}

void test_recovery(const std::filesystem::path& root) {
    const auto abandoned = root / (std::string{".roc-"}+kTaskId+"-123e4567-e89b-42d3-a456-426614174043.part");
    const auto unrelated = root / "keep.part";
    {
        std::ofstream output{abandoned};
        output << "partial";
    }
#ifndef _WIN32
    EXPECT(::chmod(abandoned.c_str(),0600)==0);
#endif
    {
        std::ofstream output{unrelated};
        output << "keep";
    }
    const auto recovered = roc::vehicle::recover_file_sink_parts(root);
    EXPECT(recovered);
    if (recovered) {
        EXPECT(recovered.value() == 1);
    }
    EXPECT(!std::filesystem::exists(abandoned));
    EXPECT(std::filesystem::exists(unrelated));
}

void test_unwritable_artifact_directory(const std::filesystem::path& root) {
#ifndef _WIN32
    std::error_code ignored;
    std::filesystem::create_directories(root, ignored);
    EXPECT(::chmod(root.c_str(), 0500) == 0);
    const std::string content = "artifact-data";
    const auto manifest = manifest_for(
        roc::vehicle::ResourceType::map, content, "image/png");
    const auto sink = roc::vehicle::FileSink::create(root, manifest);
    EXPECT(!sink);
    EXPECT(::chmod(root.c_str(), 0700) == 0);
    const auto recovered = roc::vehicle::recover_file_sink_parts(root);
    EXPECT(recovered);
    if (recovered) {
        EXPECT(recovered.value() == 0);
    }
#else
    static_cast<void>(root);
#endif
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-file-sink-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);
    test_map_atomic_commit(root / "map", "image/png");
    test_map_atomic_commit(root / "jpeg", "image/jpeg");
    for (const auto* media_type : {"image/png", "image/jpeg"}) {
        const auto suffix = std::string{media_type}.substr(6);
        test_hash_failure_cleans_part(root / ("bad-hash-" + suffix), media_type);
        test_http_metadata_failures(root / ("bad-metadata-" + suffix), media_type);
    }
    test_invalid_existing_file_is_replaced(root / "replace");
    test_road_validation_and_commit(root / "road");
    test_recovery(root / "map");
    test_unwritable_artifact_directory(root / "unwritable");
    std::filesystem::remove_all(root, ignored);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
