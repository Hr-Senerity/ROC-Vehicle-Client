#pragma once
#include "roc/vehicle/package_sink.hpp"
#include "roc/vehicle/crypto.hpp"
#include <fstream>
#include <map>

namespace package_test {
using namespace roc::vehicle;
inline constexpr auto task_id="123e4567-e89b-12d3-a456-426614174010";
struct Fixture { ArtifactManifest manifest; std::map<std::string,std::string> bytes; };
inline Fixture fixture(std::string format="pgm-yaml") {
    Fixture value; auto& m=value.manifest;
    m.task_id=task_id; m.batch_id="123e4567-e89b-12d3-a456-426614174011";
    m.project_id="123e4567-e89b-12d3-a456-426614174012";
    m.resource_revision_id="123e4567-e89b-12d3-a456-426614174013";
    m.map_id="123e4567-e89b-12d3-a456-426614174014";
    m.resource_type=ResourceType::map; m.version=2; m.package_version=2;
    m.content_type="application/vnd.roc.map-package+json";
    m.package_digest_algorithm="roc-file-set-v1"; m.map_format=format;
    m.coordinate_mode="metric"; m.resolution=0.05;
    m.origin_x=1.25; m.origin_y=-2.5; m.origin_theta=0.3; m.image_width=2; m.image_height=1;
    std::string pgm="P5\n2 1\n255\n"; pgm.push_back('\0'); pgm.push_back(static_cast<char>(255));
    const std::string yaml="image: original.pgm\nresolution: 0.05\norigin: [1.25, -2.5, 0.3]\n";
    const auto add=[&](const std::string& role,const std::string& name,const std::string& mime,
                       const std::string& content,const std::string& id) {
        ArtifactFile file{id,role,name,mime,content.size(),sha256_hex(content).value(),
            task_artifact_file_target(task_id,id)};
        m.files.push_back(file); value.bytes[id]=content; m.byte_size+=content.size();
    };
    if (format=="pgm-yaml") {
        add("pgm","original.pgm","image/x-portable-graymap",pgm,"123e4567-e89b-12d3-a456-426614174040");
        add("yaml","original.yaml","application/yaml",yaml,"123e4567-e89b-12d3-a456-426614174041");
    } else {
        // Opaque artifact bytes: no decoder/format conversion is part of this library.
        add("image","original."+format,format=="png"?"image/png":"image/jpeg",
            "original image bytes","123e4567-e89b-12d3-a456-426614174042");
    }
    m.sha256=map_package_digest(m.files).value(); return value;
}
inline ArtifactHttpMetadata metadata(const ArtifactFile& file) {
    return {file.content_type,file.sha256,file.byte_size,file.byte_size};
}
inline Result<void> write_and_seal(PackageSink& sink, const Fixture& fixture_value, const ArtifactFile& file) {
    const auto& bytes=fixture_value.bytes.at(file.id);
    const auto result=sink.write(file.id,reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());
    if (!result) return result.error();
    return sink.seal_file(file.id,metadata(file));
}
inline Result<std::unique_ptr<PackageSink>> prepared(const std::filesystem::path& root,const Fixture& value) {
    auto sink=PackageSink::create(root,value.manifest); if (!sink) return sink.error();
    for (auto it=value.manifest.files.rbegin();it!=value.manifest.files.rend();++it) {
        const auto sealed=write_and_seal(*sink.value(),value,*it); if (!sealed) return sealed.error();
    }
    const auto sealed=sink.value()->seal(); if (!sealed) return sealed.error();
    return sink;
}
inline std::string read(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
inline std::size_t published_count(const std::filesystem::path& root) {
    std::size_t count=0;
    for (const auto& entry:std::filesystem::directory_iterator(root))
        if (entry.path().filename().string().find("map-package-")==0) ++count;
    return count;
}
}
