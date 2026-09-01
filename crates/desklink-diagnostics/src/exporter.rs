//! Local diagnostics bundle export helpers.

use crate::DiagnosticSnapshot;

/// Exports diagnostics snapshots without performing network upload.
///
/// The actual archive writer is intentionally kept behind this API so future
/// formats can be added without changing callers.
pub struct DiagnosticsExporter;

impl DiagnosticsExporter {
    pub fn validate(snapshot: &DiagnosticSnapshot) -> bool {
        !snapshot.session.state.is_empty()
    }
}
