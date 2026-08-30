#include "active_session.h"

#include <windows.h>
#include <wtsapi32.h>

#include <algorithm>
#include <vector>

namespace desklink {
namespace {

bool SessionHasUser(DWORD session_id) {
  if (session_id == kInvalidInteractiveSession) return false;

  LPWSTR username = nullptr;
  DWORD bytes = 0;
  const BOOL queried = WTSQuerySessionInformationW(
      WTS_CURRENT_SERVER_HANDLE,
      session_id,
      WTSUserName,
      &username,
      &bytes);
  const bool has_user = queried && username != nullptr &&
                        bytes >= sizeof(wchar_t) && username[0] != L'\0';
  if (username) WTSFreeMemory(username);
  return has_user;
}

}  // namespace

uint32_t SelectActiveInteractiveSession(
    uint32_t console_session_id,
    const std::vector<InteractiveSessionCandidate>& candidates) {
  for (const auto& candidate : candidates) {
    if (candidate.session_id == console_session_id &&
        candidate.active && candidate.has_user) {
      return console_session_id;
    }
  }

  uint32_t selected = kInvalidInteractiveSession;
  for (const auto& candidate : candidates) {
    if (!candidate.active || !candidate.has_user) continue;
    if (selected == kInvalidInteractiveSession || candidate.session_id < selected) {
      selected = candidate.session_id;
    }
  }
  return selected;
}

uint32_t FindActiveInteractiveSession() {
  const DWORD console_session = WTSGetActiveConsoleSessionId();

  PWTS_SESSION_INFOW sessions = nullptr;
  DWORD count = 0;
  if (!WTSEnumerateSessionsW(
          WTS_CURRENT_SERVER_HANDLE,
          0,
          1,
          &sessions,
          &count)) {
    // If Terminal Services enumeration itself fails, retain the historical
    // console behavior rather than leaving a normally logged-in workstation
    // offline solely because the richer discovery path is unavailable.
    return SessionHasUser(console_session)
        ? static_cast<uint32_t>(console_session)
        : kInvalidInteractiveSession;
  }

  std::vector<InteractiveSessionCandidate> candidates;
  candidates.reserve(count);
  for (DWORD index = 0; index < count; ++index) {
    const auto& session = sessions[index];
    candidates.push_back({
        static_cast<uint32_t>(session.SessionId),
        session.State == WTSActive,
        SessionHasUser(session.SessionId),
    });
  }
  if (sessions) WTSFreeMemory(sessions);

  return SelectActiveInteractiveSession(
      static_cast<uint32_t>(console_session),
      candidates);
}

}  // namespace desklink
