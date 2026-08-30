#pragma once

#include <windows.h>

#include <string>

namespace desklink {

// Verifies that the supported Windows Secure Attention Sequence API can be
// resolved from the system Sas.dll. This does not change local security policy.
bool SecureAttentionSequenceApiAvailable(std::wstring* error);

// Called only by the LocalSystem DeskLink Service after the named-pipe client
// PID/SID has already been authenticated. The Service temporarily impersonates
// that exact pipe client so Windows targets the SAS at the Agent's user session.
//
// SendSAS itself has no success return value: true means DeskLink successfully
// dispatched the request to Windows. Windows may still ignore it if the local
// "Software Secure Attention Sequence" policy does not allow Services.
bool SendSecureAttentionSequenceForPipeClient(
    HANDLE pipe,
    std::wstring* error);

}  // namespace desklink
