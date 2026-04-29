/*
 * dwg_to_scs — DWG to SCS converter using HOOPS Exchange SDK.
 *
 * Usage:
 *   dwg_to_scs <input.dwg> <output.scs>
 *   dwg_to_scs --batch <input_dir> <output_dir>
 *
 * Environment variables:
 *   HOOPS_EXCHANGE_ROOT  — SDK root directory (contains include/ and bin/)
 *   HOOPS_EXCHANGE_LICENSE — license key (overrides hoops_license.h)
 */

#define INITIALIZE_A3D_API
#include <A3DSDKIncludes.h>
#include <A3DSDKInternalConvert.hxx>
#include "hoops_license.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

// Convert fs::path to the platform path type (wide on Windows, UTF-8 on Linux)
static path_t to_platform_path(const fs::path& p) {
#ifdef _MSC_VER
    return p.wstring();
#else
    return p.string();
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

static A3DStatus convert_single(A3DSDKHOOPSExchangeLoader& loader,
                                 const path_t& input,
                                 const path_t& output,
                                 bool safe_mode) {
    A3DImport sImport(PATH_CSTR(input));
    if (safe_mode) {
        // Minimal loading: 2D wireframes only, no solids/surfaces/PMI
        sImport.m_sLoadData.m_sGeneral.m_eReadingMode2D3D = kA3DRead_Drawings;
        sImport.m_sLoadData.m_sGeneral.m_eReadGeomTessMode = kA3DReadTessOnly;
        sImport.m_sLoadData.m_sGeneral.m_bReadSolids = false;
        sImport.m_sLoadData.m_sGeneral.m_bReadSurfaces = false;
        sImport.m_sLoadData.m_sGeneral.m_bReadWireframes = true;
        sImport.m_sLoadData.m_sGeneral.m_bReadPmis = false;
        sImport.m_sLoadData.m_sGeneral.m_bReadAttributes = false;
        sImport.m_sLoadData.m_sGeneral.m_bReadHiddenObjects = false;
        sImport.m_sLoadData.m_sTessellation.m_eTessellationLevelOfDetail = kA3DTessLODLow;
    } else {
        sImport.m_sLoadData.m_sGeneral.m_eReadingMode2D3D = kA3DRead_3D;
        sImport.m_sLoadData.m_sGeneral.m_eReadGeomTessMode = kA3DReadGeomAndTess;
        sImport.m_sLoadData.m_sGeneral.m_bReadSolids = true;
        sImport.m_sLoadData.m_sGeneral.m_bReadSurfaces = true;
        sImport.m_sLoadData.m_sGeneral.m_bReadWireframes = true;
        sImport.m_sLoadData.m_sGeneral.m_bReadPmis = true;
        sImport.m_sLoadData.m_sGeneral.m_bReadAttributes = true;
        sImport.m_sLoadData.m_sTessellation.m_eTessellationLevelOfDetail = kA3DTessLODMedium;
    }

    A3DExport sExport(PATH_CSTR(output));

    A3DStatus ret = loader.Convert(sImport, sExport);
    if (ret == A3D_SUCCESS) {
        auto fsize = fs::exists(output) ? fs::file_size(output) : 0;
        printf("  OK: %s (%zu bytes)\n", to_utf8(output).c_str(), fsize);
    } else {
        const char* msg = A3DMiscGetErrorMsg ? A3DMiscGetErrorMsg(ret) : nullptr;
        fprintf(stderr, "  FAIL: %s (error %d%s%s)\n",
                to_utf8(input).c_str(), ret,
                msg ? ": " : "", msg ? msg : "");
    }
    return ret;
}

static int batch_convert(A3DSDKHOOPSExchangeLoader& loader,
                          const path_t& input_dir,
                          const path_t& output_dir,
                          bool safe_mode = false) {
    if (!fs::exists(output_dir)) {
        fs::create_directories(output_dir);
    }

    int ok = 0, fail = 0;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        if (ext != L".dwg") continue;

        auto input = to_platform_path(entry.path());
        auto output = to_platform_path(fs::path(output_dir) / entry.path().stem());
#ifdef _MSC_VER
        output += L".scs";
#else
        output += ".scs";
#endif

        printf("[%d] ", ok + fail + 1);
        A3DStatus ret = convert_single(loader, input, output, safe_mode);
        if (ret == A3D_SUCCESS || ret == A3D_LOAD_MISSING_COMPONENTS) {
            ok++;
        } else {
            fail++;
        }
    }

    printf("\nBatch done: %d succeeded, %d failed\n", ok, fail);
    return fail > 0 ? 1 : 0;
}

static void print_usage(const char* prog) {
    printf("Usage:\n");
    printf("  %s <input.dwg> <output.scs>\n", prog);
    printf("  %s --batch <input_dir> <output_dir>\n", prog);
    printf("\nEnvironment:\n");
    printf("  HOOPS_EXCHANGE_ROOT     - SDK root directory\n");
    printf("  HOOPS_EXCHANGE_LICENSE  - license key (optional, uses hoops_license.h)\n");
}

#ifdef _MSC_VER
int wmain(int argc, wchar_t* wargv[]) {
#else
int main(int argc, char* argv[]) {
#endif

    if (argc < 2) {
#ifdef _MSC_VER
        char prog[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, wargv[0], -1, prog, MAX_PATH, nullptr, nullptr);
        print_usage(prog);
#else
        print_usage(argv[0]);
#endif
        return 1;
    }

    bool batch_mode = false;
    bool safe_mode = false;
    int arg_offset = 0;

#ifdef _MSC_VER
    for (int i = 1; i < argc; i++) {
        if (wcscmp(wargv[i], L"--batch") == 0) batch_mode = true;
        else if (wcscmp(wargv[i], L"--safe") == 0) safe_mode = true;
        else break;
        arg_offset = i;
    }
#else
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--batch") == 0) batch_mode = true;
        else if (strcmp(argv[i], "--safe") == 0) safe_mode = true;
        else break;
        arg_offset = i;
    }
#endif

    int min_args = 2 + arg_offset + (batch_mode ? 1 : 0);  // prog + flags + [batchdir] + args
    if (batch_mode && argc < arg_offset + 3) {
        fprintf(stderr, "Error: --batch requires <input_dir> <output_dir>\n");
        return 1;
    }
    if (!batch_mode && argc < arg_offset + 2) {
        fprintf(stderr, "Error: need <input.dwg> <output.scs>\n");
        return 1;
    }

    if (safe_mode) printf("Safe mode enabled (2D, tess-only, minimal)\n");

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

    if (batch_mode) {
#ifdef _MSC_VER
        path_t input_dir = wargv[arg_offset + 1];
        path_t output_dir = wargv[arg_offset + 2];
#else
        path_t input_dir = argv[arg_offset + 1];
        path_t output_dir = argv[arg_offset + 2];
#endif
        return batch_convert(loader, input_dir, output_dir, safe_mode);
    } else {
#ifdef _MSC_VER
        path_t input = wargv[arg_offset + 1];
        path_t output = wargv[arg_offset + 2];
#else
        path_t input = argv[arg_offset + 1];
        path_t output = argv[arg_offset + 2];
#endif
        A3DStatus ret = convert_single(loader, input, output, safe_mode);
        return (ret == A3D_SUCCESS || ret == A3D_LOAD_MISSING_COMPONENTS) ? 0 : 1;
    }
}
