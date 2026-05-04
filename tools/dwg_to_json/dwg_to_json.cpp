/*
 * dwg_to_json — DWG to JSON converter using HOOPS Exchange SDK.
 *
 * Exports structured entity tree from DWG files for comparison with
 * our self-developed DWG parser's output.
 *
 * Usage:
 *   dwg_to_json <input.dwg> <output.json>
 *
 * Environment variables:
 *   HOOPS_EXCHANGE_ROOT  — SDK root directory (contains include/ and bin/)
 *   HOOPS_EXCHANGE_LICENSE — license key (overrides hoops_license.h)
 */

#define INITIALIZE_A3D_API
#include <A3DSDKIncludes.h>
#include <A3DSDKInternalConvert.hxx>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "entity_visitor.h"
#include "json_writer.h"
#include "hoops_license.h"

#ifdef _MSC_VER
#include <windows.h>
#endif

namespace fs = std::filesystem;

// --- Platform path type ---
#ifdef _MSC_VER
using path_t = std::wstring;
#define PATH_CSTR(p) (p).c_str()
#else
using path_t = std::string;
#define PATH_CSTR(p) (p).c_str()
#endif

static const char* get_license() {
    const char* env = std::getenv("HOOPS_EXCHANGE_LICENSE");
    if (env && env[0] != '\0') return env;
    return HOOPS_LICENSE;
}

static path_t get_sdk_bin_path() {
    const char* root = std::getenv("HOOPS_EXCHANGE_ROOT");
    if (!root || root[0] == '\0') {
        root = std::getenv("A3DLIBS_ROOT");
    }
    std::string bin;
    if (root && root[0] != '\0') {
        bin = std::string(root) + "/bin/win64_v142";
    }
#ifdef HOOPS_SDK_BIN_DIR
    else {
        bin = HOOPS_SDK_BIN_DIR;
    }
#endif
#ifdef _MSC_VER
    if (bin.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, bin.c_str(), -1, nullptr, 0);
    std::wstring wbin;
    wbin.resize(wlen - 1);
    MultiByteToWideChar(CP_UTF8, 0, bin.c_str(), -1, wbin.data(), wlen);
    return wbin;
#else
    return bin;
#endif
}

static std::string to_utf8(const fs::path& p) {
#ifdef _MSC_VER
    std::wstring ws = p.wstring();
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s;
    s.resize(len - 1);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
#else
    return p.string();
#endif
}

static int convert_single(const path_t& input, const path_t& output) {
    // Initialize HOOPS Exchange
    path_t bin_path = get_sdk_bin_path();
    if (bin_path.empty()) {
        fprintf(stderr, "Error: HOOPS_EXCHANGE_ROOT not set.\n");
        return 1;
    }

    const char* license = get_license();
    A3DSDKHOOPSExchangeLoader loader(PATH_CSTR(bin_path), license);

    if (loader.m_eSDKStatus != A3D_SUCCESS) {
        const char* msg = A3DMiscGetErrorMsg ? A3DMiscGetErrorMsg(loader.m_eSDKStatus) : nullptr;
        fprintf(stderr, "Error: HOOPS Exchange init failed (status %d%s%s)\n",
                loader.m_eSDKStatus, msg ? ": " : "", msg ? msg : "");
        return 1;
    }

    printf("HOOPS Exchange initialized successfully\n");

    // Configure import for Model Space only (skip Drawing/Paper Space)
    A3DImport sImport(PATH_CSTR(input));
    sImport.m_sLoadData.m_sGeneral.m_eReadingMode2D3D = kA3DRead_Both;
    sImport.m_sLoadData.m_sGeneral.m_eReadGeomTessMode = kA3DReadGeomAndTess;
    sImport.m_sLoadData.m_sGeneral.m_bReadSolids = true;
    sImport.m_sLoadData.m_sGeneral.m_bReadSurfaces = true;
    sImport.m_sLoadData.m_sGeneral.m_bReadWireframes = true;
    sImport.m_sLoadData.m_sGeneral.m_bReadPmis = true;
    sImport.m_sLoadData.m_sGeneral.m_bReadAttributes = true;
    sImport.m_sLoadData.m_sGeneral.m_bReadHiddenObjects = true;
    sImport.m_sLoadData.m_sGeneral.m_eDefaultUnit = kA3DUnitUnknown;

    // Import the file
    A3DStatus ret = loader.Import(sImport);
    if (ret != A3D_SUCCESS && ret != A3D_LOAD_MISSING_COMPONENTS) {
        const char* msg = A3DMiscGetErrorMsg ? A3DMiscGetErrorMsg(ret) : nullptr;
        fprintf(stderr, "Error: Failed to import %s (error %d%s%s)\n",
                to_utf8(input).c_str(), ret,
                msg ? ": " : "", msg ? msg : "");
        return 1;
    }

    printf("File imported successfully\n");

    // Traverse the entity tree
    // Note: m_psModelFile is private in A3DSDKHOOPSExchangeLoader
    // We need to use A3DAsmModelFileLoadFromFile directly instead
    // Actually, let's use the internal pointer after Import

    // Since we can't access m_psModelFile directly, let's use the approach
    // of loading directly with A3DAsmModelFileLoadFromFile
    // Note: A3DAsmModelFileLoadFromFile expects UTF-8 path, not wide char
    A3DAsmModelFile* model_file = nullptr;
    std::string input_utf8 = to_utf8(fs::path(input));
    ret = A3DAsmModelFileLoadFromFile(input_utf8.c_str(), &sImport.m_sLoadData, &model_file);
    if (ret != A3D_SUCCESS) {
        const char* msg = A3DMiscGetErrorMsg ? A3DMiscGetErrorMsg(ret) : nullptr;
        fprintf(stderr, "Error: A3DAsmModelFileLoadFromFile failed (error %d%s%s)\n",
                ret, msg ? ": " : "", msg ? msg : "");
        return 1;
    }

    // Create visitor and traverse
    EntityVisitor visitor;
    visitor.traverse(model_file);

    printf("Traversed %d entities\n", visitor.total_entities());

    // Write JSON output
    FILE* out = nullptr;
#ifdef _MSC_VER
    out = _wfopen(output.c_str(), L"w");
#else
    out = fopen(to_utf8(fs::path(output)).c_str(), "w");
#endif
    if (!out) {
        fprintf(stderr, "Error: Cannot open output file %s\n", to_utf8(fs::path(output)).c_str());
        A3DAsmModelFileDelete(model_file);
        return 1;
    }

    JsonWriter writer(out);
    visitor.write_json(writer, to_utf8(fs::path(input)).c_str());
    fclose(out);

    // Cleanup
    A3DAsmModelFileDelete(model_file);

    auto fsize = fs::exists(output) ? fs::file_size(output) : 0;
    printf("OK: %s (%zu bytes)\n", to_utf8(fs::path(output)).c_str(), fsize);

    return 0;
}

static void print_usage(const char* prog) {
    printf("Usage:\n");
    printf("  %s <input.dwg> <output.json>\n", prog);
    printf("\nEnvironment:\n");
    printf("  HOOPS_EXCHANGE_ROOT     - SDK root directory\n");
    printf("  HOOPS_EXCHANGE_LICENSE  - license key\n");
}

#ifdef _MSC_VER
int wmain(int argc, wchar_t* wargv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    if (argc < 3) {
#ifdef _MSC_VER
        char prog[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, wargv[0], -1, prog, MAX_PATH, nullptr, nullptr);
        print_usage(prog);
#else
        print_usage(argv[0]);
#endif
        return 1;
    }

#ifdef _MSC_VER
    path_t input = wargv[1];
    path_t output = wargv[2];
#else
    path_t input = argv[1];
    path_t output = argv[2];
#endif

    return convert_single(input, output);
}
