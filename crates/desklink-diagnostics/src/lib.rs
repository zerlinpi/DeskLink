//! Diagnostics primitives for DeskLink.
//!
//! This crate keeps collection, redaction and export layers separated so
//! diagnostics cannot accidentally expose credentials.

mod exporter;
mod redact;
mod snapshot;

pub use exporter::{DiagnosticsExportError, DiagnosticsExporter};
pub use redact::{
    is_sensitive_key, redact_value, DiagnosticsRedactor, SecretKind, REDACTED,
};
pub use snapshot::{
    DiagnosticError, DiagnosticLogEvent, DiagnosticSnapshot, DisplayDiagnostics,
    LogsDiagnostics, MediaDiagnostics, NetworkDiagnostics, RecoveryEvent,
    ServiceDiagnostics, SessionDiagnostics, SystemDiagnostics, TimelineEvent,
    DIAGNOSTICS_SCHEMA_VERSION,
};
