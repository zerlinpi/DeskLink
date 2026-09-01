use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiagnosticSnapshot {
    pub version: String,
    pub session: Option<SessionDiagnostics>,
    pub errors: Vec<DiagnosticError>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SessionDiagnostics {
    pub state: String,
    pub session_generation: u64,
    pub peer_generation: u64,
    pub channel_generation: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiagnosticError {
    pub component: String,
    pub message: String,
}
