//! Secret redaction utilities for diagnostics output.

const REDACTED: &str = "***REDACTED***";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SecretKind {
    AccessCode,
    DeviceCredential,
    ControllerToken,
    TurnSecret,
    SignalToken,
}

pub fn redact_value(_kind: SecretKind, _value: &str) -> &'static str {
    REDACTED
}

pub fn is_sensitive_key(key: &str) -> bool {
    let key = key.to_ascii_lowercase();

    key.contains("token")
        || key.contains("secret")
        || key.contains("credential")
        || key.contains("access_code")
        || key.contains("accesscode")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sensitive_keys_are_detected() {
        assert!(is_sensitive_key("controller_token"));
        assert!(is_sensitive_key("turn_secret"));
        assert!(!is_sensitive_key("session_state"));
    }

    #[test]
    fn values_are_redacted() {
        assert_eq!(redact_value(SecretKind::SignalToken, "abc"), REDACTED);
    }
}
