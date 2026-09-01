//! Secret redaction utilities for diagnostics output.

use serde::Serialize;
use serde_json::Value;

pub const REDACTED: &str = "***REDACTED***";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SecretKind {
    AccessCode,
    DeviceCredential,
    ControllerToken,
    TurnSecret,
    SignalSecret,
    SignalToken,
    PrivateKey,
    DpapiBlob,
}

pub fn redact_value(_kind: SecretKind, _value: &str) -> &'static str {
    REDACTED
}

pub fn is_sensitive_key(key: &str) -> bool {
    let normalized: String = key
        .chars()
        .filter(|character| character.is_ascii_alphanumeric())
        .map(|character| character.to_ascii_lowercase())
        .collect();

    [
        "accesscode",
        "authorization",
        "controllertoken",
        "cookie",
        "credential",
        "devicecredential",
        "dpapiblob",
        "nonce",
        "password",
        "passphrase",
        "privatekey",
        "proof",
        "secret",
        "signalsecret",
        "signaltoken",
        "token",
        "turnpassword",
        "turnsecret",
    ]
    .iter()
    .any(|needle| normalized.contains(needle))
}

/// The only supported gateway from a typed snapshot to exported JSON.
///
/// In addition to sensitive JSON keys, callers can register the exact secret
/// values already resident in their process. Those values are removed from
/// free-form log messages even when the message used an unexpected label.
#[derive(Debug, Clone, Default)]
pub struct DiagnosticsRedactor {
    known_secrets: Vec<String>,
}

impl DiagnosticsRedactor {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn with_secret(mut self, _kind: SecretKind, value: impl Into<String>) -> Self {
        let value = value.into();
        if value.len() >= 4 && !self.known_secrets.iter().any(|known| known == &value) {
            self.known_secrets.push(value);
        }
        self
    }

    pub fn redact_serializable<T: Serialize>(&self, value: &T) -> serde_json::Result<Value> {
        let mut json = serde_json::to_value(value)?;
        self.redact_json(&mut json);
        Ok(json)
    }

    pub fn redact_json(&self, value: &mut Value) {
        match value {
            Value::Object(fields) => {
                for (key, child) in fields {
                    if is_sensitive_key(key) {
                        *child = Value::String(REDACTED.to_owned());
                    } else {
                        self.redact_json(child);
                    }
                }
            }
            Value::Array(items) => {
                for item in items {
                    self.redact_json(item);
                }
            }
            Value::String(text) => *text = self.redact_text(text),
            Value::Null | Value::Bool(_) | Value::Number(_) => {}
        }
    }

    pub fn redact_text(&self, text: &str) -> String {
        if text.to_ascii_uppercase().contains("PRIVATE KEY-----") {
            return REDACTED.to_owned();
        }

        let mut redacted = text.to_owned();
        for secret in &self.known_secrets {
            redacted = redacted.replace(secret, REDACTED);
        }
        redacted = redact_bearer_tokens(redacted);

        for label in [
            "access code",
            "access-code",
            "access_code",
            "authorization",
            "controller token",
            "controller-token",
            "controller_token",
            "device credential",
            "device-credential",
            "device_credential",
            "dpapi blob",
            "dpapi-blob",
            "dpapi_blob",
            "private key",
            "private-key",
            "private_key",
            "signal secret",
            "signal-secret",
            "signal_secret",
            "signal token",
            "signal-token",
            "signal_token",
            "turn password",
            "turn-password",
            "turn_password",
            "turn secret",
            "turn-secret",
            "turn_secret",
        ] {
            redacted = redact_labeled_values(redacted, label);
        }

        redacted
    }
}

fn redact_labeled_values(mut text: String, label: &str) -> String {
    let mut cursor = 0;
    loop {
        let lower = text.to_ascii_lowercase();
        let Some(relative) = lower[cursor..].find(label) else {
            break;
        };
        let label_start = cursor + relative;
        let mut separator = label_start + label.len();
        let bytes = text.as_bytes();

        while separator < bytes.len() && bytes[separator].is_ascii_whitespace() {
            separator += 1;
        }
        if separator < bytes.len() && matches!(bytes[separator], b'\'' | b'\"') {
            separator += 1;
            while separator < bytes.len() && bytes[separator].is_ascii_whitespace() {
                separator += 1;
            }
        }
        if separator >= bytes.len() || !matches!(bytes[separator], b'=' | b':') {
            cursor = label_start + label.len();
            continue;
        }

        let mut value_start = separator + 1;
        while value_start < bytes.len() && bytes[value_start].is_ascii_whitespace() {
            value_start += 1;
        }
        let quote = if value_start < bytes.len()
            && matches!(bytes[value_start], b'\'' | b'\"')
        {
            let quote = Some(bytes[value_start]);
            value_start += 1;
            quote
        } else {
            None
        };

        let mut value_end = value_start;
        while value_end < bytes.len() {
            let byte = bytes[value_end];
            let finished = quote.map_or_else(
                || byte.is_ascii_whitespace() || matches!(byte, b',' | b';' | b'&' | b'}'),
                |expected| byte == expected,
            );
            if finished {
                break;
            }
            value_end += 1;
        }

        if value_end == value_start {
            cursor = value_start;
            continue;
        }
        text.replace_range(value_start..value_end, REDACTED);
        cursor = value_start + REDACTED.len();
    }
    text
}

fn redact_bearer_tokens(mut text: String) -> String {
    let mut cursor = 0;
    loop {
        let lower = text.to_ascii_lowercase();
        let Some(relative) = lower[cursor..].find("bearer ") else {
            break;
        };
        let value_start = cursor + relative + "bearer ".len();
        let bytes = text.as_bytes();
        let mut value_end = value_start;
        while value_end < bytes.len()
            && !bytes[value_end].is_ascii_whitespace()
            && !matches!(bytes[value_end], b',' | b';' | b'&' | b'\'' | b'\"')
        {
            value_end += 1;
        }
        if value_end == value_start {
            cursor = value_start;
            continue;
        }
        text.replace_range(value_start..value_end, REDACTED);
        cursor = value_start + REDACTED.len();
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn sensitive_keys_are_detected() {
        assert!(is_sensitive_key("controller_token"));
        assert!(is_sensitive_key("turn_secret"));
        assert!(is_sensitive_key("Access-Code"));
        assert!(is_sensitive_key("privateKey"));
        assert!(is_sensitive_key("dpapi_blob"));
        assert!(!is_sensitive_key("session_state"));
    }

    #[test]
    fn values_are_redacted() {
        assert_eq!(redact_value(SecretKind::SignalToken, "abc"), REDACTED);
    }

    #[test]
    fn recursively_redacts_sensitive_fields_and_known_values() {
        let mut value = json!({
            "controller_token": "controller-secret",
            "nested": [{
                "message": "failed with device_credential=known-device-secret",
                "safe": "known-device-secret",
            }],
        });
        let redactor = DiagnosticsRedactor::new().with_secret(
            SecretKind::DeviceCredential,
            "known-device-secret",
        );

        redactor.redact_json(&mut value);
        let serialized = serde_json::to_string(&value).expect("serialize redacted value");

        assert!(!serialized.contains("controller-secret"));
        assert!(!serialized.contains("known-device-secret"));
        assert!(serialized.contains(REDACTED));
    }

    #[test]
    fn redacts_embedded_json_style_and_bearer_values() {
        let redactor = DiagnosticsRedactor::new();
        let text = redactor.redact_text(
            r#"request {"turn_secret":"turn-value"} Authorization: Bearer bearer-value"#,
        );

        assert!(!text.contains("turn-value"));
        assert!(!text.contains("bearer-value"));
        assert!(text.matches(REDACTED).count() >= 2);
    }

    #[test]
    fn drops_private_key_blocks_as_a_whole() {
        let redactor = DiagnosticsRedactor::new();
        assert_eq!(
            redactor.redact_text("-----BEGIN PRIVATE KEY----- secret"),
            REDACTED
        );
    }
}
