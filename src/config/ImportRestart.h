#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <rapidjson/document.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace ImportRestart
{
struct Target
{
    std::filesystem::path executable;
    std::filesystem::path mlc;
    std::string account;
};

inline void RequirePlainPath(const std::filesystem::path& path)
{
    if (!path.is_absolute()) throw std::runtime_error("Import path must be absolute.");
#ifdef _WIN32
    if (path.native().starts_with(L"\\\\")) throw std::runtime_error("Import must use a local drive.");
#endif
    for (auto parent = path; !parent.empty();)
    {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(parent.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Linked import paths are not accepted.");
#else
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(parent)))
            throw std::runtime_error("Linked import paths are not accepted.");
#endif
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
}

template<class HashFile>
Target Validate(const std::filesystem::path& root, HashFile hashFile)
{
    RequirePlainPath(root);
    const auto receipt = root / "IMPORT-COMPLETE.json";
    RequirePlainPath(receipt);
    if (!std::filesystem::is_regular_file(receipt) || std::filesystem::file_size(receipt) > 1024 * 1024)
        throw std::runtime_error("Missing or oversized import completion receipt.");
    std::ifstream file(receipt, std::ios::binary);
    const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    rapidjson::Document document;
    document.Parse(json.data(), json.size());
    if (document.HasParseError() || !document.IsObject()) throw std::runtime_error("Invalid import receipt.");
    const auto stringField = [&document](const char* name) -> std::string {
        if (!document.HasMember(name) || !document[name].IsString()) throw std::runtime_error("Incomplete import receipt.");
        return {document[name].GetString(), document[name].GetStringLength()};
    };
    if (stringField("status") != "private_import_complete_not_hardware_verified" ||
        !document.HasMember("full_mlc") || !document["full_mlc"].IsBool() || !document["full_mlc"].GetBool() ||
        !document.HasMember("main_release_modified") || !document["main_release_modified"].IsBool() || document["main_release_modified"].GetBool())
        throw std::runtime_error("The import is not a complete separate full-MLC setup.");
    const auto account = stringField("account");
    if (account.size() != 8 || account[0] != '8' || account == "80000000" ||
        !std::all_of(account.begin(), account.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }))
        throw std::runtime_error("Invalid imported account identity.");
    const auto hash = stringField("cemu_sha256");
    if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }))
        throw std::runtime_error("Invalid imported executable checksum.");
    Target target{root / "Cemu_release.exe", root / "mlc01", account};
    for (const auto& path : {target.executable, target.mlc, root / "settings.xml", root / "otp.bin", root / "seeprom.bin"}) RequirePlainPath(path);
    if (!std::filesystem::is_directory(target.mlc) || !std::filesystem::is_regular_file(target.executable) ||
        std::filesystem::file_size(root / "otp.bin") != 1024 || std::filesystem::file_size(root / "seeprom.bin") != 512)
        throw std::runtime_error("Imported setup is incomplete.");
    if (hashFile(target.executable) != hash) throw std::runtime_error("Imported Cemu executable changed. Restart cancelled.");
    return target;
}
}
