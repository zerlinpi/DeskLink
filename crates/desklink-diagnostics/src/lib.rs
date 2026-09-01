//! Diagnostics primitives for DeskLink.
//!
//! This crate intentionally contains only data models. Collection, redaction,
//! and exporting are separate layers so diagnostics cannot accidentally expose
//! credentials.

mod snapshot;

pub use snapshot::{DiagnosticError, DiagnosticSnapshot, SessionDiagnostics};
