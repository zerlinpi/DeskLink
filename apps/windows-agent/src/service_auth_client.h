#pragma once

#include <string>

#include "signal_token_client.h"

namespace desklink {

bool ServiceAuthBrokerConfigured();

// Requests a fresh short-lived signal token from the LocalSystem DeskLink Service.
// The long-lived device credential never enters this client path.
bool FetchServiceBrokerSignalToken(
    RuntimeSignalToken* signal_token,
    std::string* error);

}  // namespace desklink
