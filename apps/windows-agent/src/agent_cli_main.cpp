#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "diagnostics_bundle.h"

#ifndef DESKLINK_VERSION_STRING
#define DESKLINK_VERSION_STRING "unknown"
#endif

#ifndef DESKLINK_ENABLE_RUST_CORE_SHADOW
#define DESKLINK_ENABLE_RUST_CORE_SHADOW 0
#endif

int DeskLinkAgentMain(int argc, wchar_t** argv);

namespace {

int RunDiagnosticsExport() {
  desklink::DiagnosticsBundleMetadata metadata;
  metadata.version = DESKLINK_VERSION_STRING;
#ifdef NDEBUG
  metadata.build = "Release";
#else
  metadata.build = "Debug";
#endif
  metadata.commit_sha = "unknown";
#if DESKLINK_ENABLE_RUST_CORE_SHADOW
  metadata.runtime_mode = "cpp-authority;rust-shadow=on";
#else
  metadata.runtime_mode = "cpp-authority;rust-shadow=off";
#endif

  const std::filesystem::path output =
      std::filesystem::current_path() / L"desklink-diagnostics.zip";
  std::string error;
  if (!desklink::ExportLocalDiagnosticsBundle(metadata, output, &error)) {
    std::cerr << "Diagnostics export failed: " << error << "\n";
    return 1;
  }

  std::wcout << L"DeskLink diagnostics written to " << output.wstring() << L"\n";
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc > 1 && std::wstring_view(argv[1]) == L"--diagnostics") {
    if (argc != 2) {
      std::wcerr << L"Usage: desklink-agent --diagnostics\n";
      return 2;
    }
    return RunDiagnosticsExport();
  }

  return DeskLinkAgentMain(argc, argv);
}
