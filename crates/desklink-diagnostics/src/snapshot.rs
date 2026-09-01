use serde::{Deserialize, Serialize};

pub const DIAGNOSTICS_SCHEMA_VERSION: u32 = 1;

fn default_schema_version() -> u32 {
    DIAGNOSTICS_SCHEMA_VERSION
}

/// One point-in-time support snapshot.
///
/// Collectors should use `None` for measurements that are unavailable. A zero
/// value is reserved for a real observed zero so support bundles never pretend
/// an inactive one-shot exporter measured a running session.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiagnosticSnapshot {
    #[serde(default = "default_schema_version")]
    pub schema_version: u32,
    #[serde(default)]
    pub generated_at_unix_ms: u64,
    pub version: String,
    #[serde(default)]
    pub build: String,
    #[serde(default)]
    pub commit_sha: String,
    #[serde(default)]
    pub runtime_mode: String,
    #[serde(default)]
    pub system: SystemDiagnostics,
    pub session: Option<SessionDiagnostics>,
    #[serde(default)]
    pub network: NetworkDiagnostics,
    #[serde(default)]
    pub media: MediaDiagnostics,
    #[serde(default)]
    pub service: ServiceDiagnostics,
    #[serde(default)]
    pub logs: LogsDiagnostics,
    #[serde(default)]
    pub errors: Vec<DiagnosticError>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct SystemDiagnostics {
    pub windows_version: Option<String>,
    pub cpu: Option<String>,
    pub logical_processors: Option<u32>,
    pub memory_total_bytes: Option<u64>,
    pub memory_available_bytes: Option<u64>,
    pub gpu: Option<String>,
    pub driver: Option<String>,
    #[serde(default)]
    pub displays: Vec<DisplayDiagnostics>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DisplayDiagnostics {
    pub name: String,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub refresh_hz: Option<u32>,
    pub primary: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SessionDiagnostics {
    pub state: String,
    pub session_generation: u64,
    pub peer_generation: u64,
    #[serde(default)]
    pub negotiation_generation: u64,
    #[serde(default)]
    pub recovery_generation: u64,
    #[serde(default)]
    pub operation_generation: u64,
    #[serde(default)]
    pub channel_generation: u64,
    #[serde(default)]
    pub recovery_history: Vec<RecoveryEvent>,
    #[serde(default)]
    pub connection_timeline: Vec<TimelineEvent>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RecoveryEvent {
    pub timestamp_unix_ms: u64,
    pub level: u8,
    pub action: String,
    pub outcome: String,
    pub generation: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TimelineEvent {
    pub timestamp_unix_ms: u64,
    pub state: String,
    pub event: String,
    pub generation: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct NetworkDiagnostics {
    pub signal_state: Option<String>,
    pub ice_state: Option<String>,
    pub candidate_type: Option<String>,
    pub route: Option<String>,
    pub rtt_ms: Option<f64>,
    pub packet_loss_percent: Option<f64>,
    pub jitter_ms: Option<f64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct MediaDiagnostics {
    pub capture_fps: Option<f64>,
    pub encode_fps: Option<f64>,
    pub bitrate_bps: Option<u64>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub encoder_backend: Option<String>,
    pub gpu_usage_percent: Option<f64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ServiceDiagnostics {
    pub service_state: Option<String>,
    pub agent_state: Option<String>,
    pub restart_count: Option<u64>,
    pub crash_count: Option<u64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct LogsDiagnostics {
    #[serde(default)]
    pub recent_errors: Vec<DiagnosticLogEvent>,
    #[serde(default)]
    pub warnings: Vec<DiagnosticLogEvent>,
    #[serde(default)]
    pub recovery_events: Vec<DiagnosticLogEvent>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiagnosticLogEvent {
    pub timestamp_unix_ms: u64,
    pub component: String,
    pub code: String,
    pub message: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiagnosticError {
    pub component: String,
    pub message: String,
}

impl DiagnosticSnapshot {
    pub fn new(version: impl Into<String>) -> Self {
        Self {
            schema_version: DIAGNOSTICS_SCHEMA_VERSION,
            version: version.into(),
            ..Self::default()
        }
    }
}

impl Default for DiagnosticSnapshot {
    fn default() -> Self {
        Self {
            schema_version: DIAGNOSTICS_SCHEMA_VERSION,
            generated_at_unix_ms: 0,
            version: String::new(),
            build: String::new(),
            commit_sha: String::new(),
            runtime_mode: String::new(),
            system: SystemDiagnostics::default(),
            session: None,
            network: NetworkDiagnostics::default(),
            media: MediaDiagnostics::default(),
            service: ServiceDiagnostics::default(),
            logs: LogsDiagnostics::default(),
            errors: Vec::new(),
        }
    }
}
