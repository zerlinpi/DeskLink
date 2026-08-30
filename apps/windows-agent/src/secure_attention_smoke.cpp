#include "secure_attention.h"

#include <windows.h>

#include <iostream>
#include <string>

int wmain() {
  std::wstring error;
  if (!desklink::SecureAttentionSequenceApiAvailable(&error)) {
    std::wcerr << L"Secure Attention Sequence API unavailable: " << error << L"\n";
    return 1;
  }

  error.clear();
  if (desklink::SendSecureAttentionSequenceForPipeClient(INVALID_HANDLE_VALUE, &error)) {
    std::wcerr << L"Secure Attention helper accepted an invalid pipe handle\n";
    return 2;
  }
  if (error.empty()) {
    std::wcerr << L"Secure Attention helper did not explain invalid pipe rejection\n";
    return 3;
  }

  std::wcout << L"Secure Attention Sequence API resolution smoke passed.\n";
  return 0;
}
