//! Diagnostics primitives for DeskLink.
//!
//! This crate keeps collection, redaction and export layers separated so
//! diagnostics cannot accidentally expose credentials.

mod redact;
mod snapshot;
mod exporter;

pub use exporter::DiagnosticsExporter;
pub use redact::{is_sensitive_key, redact_value, SecretKind};
pub use snapshot::{DiagnosticError, DiagnosticSnapshot, SessionDiagnostics};
