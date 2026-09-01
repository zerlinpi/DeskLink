//! Local-only diagnostics bundle export helpers.

use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use crate::{DiagnosticSnapshot, DiagnosticsRedactor, DIAGNOSTICS_SCHEMA_VERSION};

const DEFAULT_ARCHIVE_NAME: &str = "desklink-diagnostics.zip";
const MAX_ENTRY_BYTES: usize = 4 * 1024 * 1024;
const MAX_ARCHIVE_BYTES: usize = 16 * 1024 * 1024;
const ZIP_UTF8_FLAG: u16 = 1 << 11;
const ZIP_VERSION_20: u16 = 20;
const ZIP_DOS_DATE_1980_01_01: u16 = (1 << 5) | 1;

#[derive(Debug)]
pub enum DiagnosticsExportError {
    InvalidSnapshot(&'static str),
    EntryTooLarge { name: String, bytes: usize },
    ArchiveTooLarge { bytes: usize },
    Io(io::Error),
    Serialization(serde_json::Error),
}

impl fmt::Display for DiagnosticsExportError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidSnapshot(message) => write!(formatter, "invalid snapshot: {message}"),
            Self::EntryTooLarge { name, bytes } => {
                write!(
                    formatter,
                    "diagnostics entry {name} is too large ({bytes} bytes)"
                )
            }
            Self::ArchiveTooLarge { bytes } => {
                write!(
                    formatter,
                    "diagnostics archive is too large ({bytes} bytes)"
                )
            }
            Self::Io(error) => write!(formatter, "diagnostics I/O failed: {error}"),
            Self::Serialization(error) => {
                write!(formatter, "diagnostics serialization failed: {error}")
            }
        }
    }
}

impl std::error::Error for DiagnosticsExportError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(error) => Some(error),
            Self::Serialization(error) => Some(error),
            Self::InvalidSnapshot(_)
            | Self::EntryTooLarge { .. }
            | Self::ArchiveTooLarge { .. } => None,
        }
    }
}

impl From<io::Error> for DiagnosticsExportError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

impl From<serde_json::Error> for DiagnosticsExportError {
    fn from(error: serde_json::Error) -> Self {
        Self::Serialization(error)
    }
}

/// Exports a fixed, redacted support bundle and never performs network I/O.
pub struct DiagnosticsExporter;

impl DiagnosticsExporter {
    pub fn validate(snapshot: &DiagnosticSnapshot) -> bool {
        Self::validate_snapshot(snapshot).is_ok()
    }

    pub fn export_local(
        snapshot: &DiagnosticSnapshot,
        directory: impl AsRef<Path>,
    ) -> Result<PathBuf, DiagnosticsExportError> {
        let output = directory.as_ref().join(DEFAULT_ARCHIVE_NAME);
        Self::export_to_path(snapshot, &output)?;
        Ok(output)
    }

    pub fn export_to_path(
        snapshot: &DiagnosticSnapshot,
        output: impl AsRef<Path>,
    ) -> Result<(), DiagnosticsExportError> {
        Self::export_with_redactor(snapshot, output, &DiagnosticsRedactor::new())
    }

    pub fn export_with_redactor(
        snapshot: &DiagnosticSnapshot,
        output: impl AsRef<Path>,
        redactor: &DiagnosticsRedactor,
    ) -> Result<(), DiagnosticsExportError> {
        Self::validate_snapshot(snapshot)?;

        let sanitized = redactor.redact_serializable(snapshot)?;
        let entries = build_entries(&sanitized)?;
        let archive = StoredZipArchive::build(entries)?;
        let output = output.as_ref();

        let file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(output)?;
        let mut pending = PendingBundle::new(output.to_path_buf(), file);
        pending.file_mut().write_all(&archive)?;
        pending.file_mut().sync_all()?;
        pending.commit();
        Ok(())
    }

    fn validate_snapshot(snapshot: &DiagnosticSnapshot) -> Result<(), DiagnosticsExportError> {
        if snapshot.schema_version != DIAGNOSTICS_SCHEMA_VERSION {
            return Err(DiagnosticsExportError::InvalidSnapshot(
                "unsupported schema version",
            ));
        }
        if snapshot.version.trim().is_empty() {
            return Err(DiagnosticsExportError::InvalidSnapshot(
                "DeskLink version is required",
            ));
        }
        if let Some(session) = &snapshot.session {
            if session.state.trim().is_empty() {
                return Err(DiagnosticsExportError::InvalidSnapshot(
                    "session state must not be empty when a session is present",
                ));
            }
        }
        Ok(())
    }
}

struct PendingBundle {
    path: PathBuf,
    file: Option<File>,
    committed: bool,
}

impl PendingBundle {
    fn new(path: PathBuf, file: File) -> Self {
        Self {
            path,
            file: Some(file),
            committed: false,
        }
    }

    fn file_mut(&mut self) -> &mut File {
        self.file.as_mut().expect("pending bundle owns its file")
    }

    fn commit(mut self) {
        drop(self.file.take());
        self.committed = true;
    }
}

impl Drop for PendingBundle {
    fn drop(&mut self) {
        if !self.committed {
            drop(self.file.take());
            let _ = fs::remove_file(&self.path);
        }
    }
}

fn build_entries(sanitized: &Value) -> Result<Vec<ArchiveEntry>, DiagnosticsExportError> {
    let schema_version = sanitized
        .get("schema_version")
        .cloned()
        .unwrap_or(Value::Null);
    let generated_at_unix_ms = sanitized
        .get("generated_at_unix_ms")
        .cloned()
        .unwrap_or(Value::Null);

    let manifest = json!({
        "schema_version": schema_version,
        "generated_at_unix_ms": generated_at_unix_ms,
        "product": "DeskLink",
        "local_export_only": true,
        "auto_upload": false,
        "redaction": "DiagnosticsRedactor",
    });
    let desklink = json!({
        "version": field(sanitized, "version"),
        "build": field(sanitized, "build"),
        "commit_sha": field(sanitized, "commit_sha"),
        "runtime_mode": field(sanitized, "runtime_mode"),
    });
    let logs = json!({
        "recent_errors": nested_field(sanitized, "logs", "recent_errors"),
        "warnings": nested_field(sanitized, "logs", "warnings"),
        "recovery_events": nested_field(sanitized, "logs", "recovery_events"),
        "collector_errors": field(sanitized, "errors"),
    });

    [
        ("manifest.json", manifest),
        ("system.json", field(sanitized, "system")),
        ("desklink.json", desklink),
        ("session.json", field(sanitized, "session")),
        ("network.json", field(sanitized, "network")),
        ("media.json", field(sanitized, "media")),
        ("service.json", field(sanitized, "service")),
        ("logs.json", logs),
    ]
    .into_iter()
    .map(|(name, value)| ArchiveEntry::from_json(name, &value))
    .collect()
}

fn field(value: &Value, name: &str) -> Value {
    value.get(name).cloned().unwrap_or(Value::Null)
}

fn nested_field(value: &Value, parent: &str, name: &str) -> Value {
    value
        .get(parent)
        .and_then(|parent| parent.get(name))
        .cloned()
        .unwrap_or(Value::Null)
}

struct ArchiveEntry {
    name: String,
    data: Vec<u8>,
}

impl ArchiveEntry {
    fn from_json(name: &str, value: &Value) -> Result<Self, DiagnosticsExportError> {
        let data = serde_json::to_vec_pretty(value)?;
        if data.len() > MAX_ENTRY_BYTES {
            return Err(DiagnosticsExportError::EntryTooLarge {
                name: name.to_owned(),
                bytes: data.len(),
            });
        }
        Ok(Self {
            name: name.to_owned(),
            data,
        })
    }
}

struct StoredZipArchive;

impl StoredZipArchive {
    fn build(entries: Vec<ArchiveEntry>) -> Result<Vec<u8>, DiagnosticsExportError> {
        let entry_count = u16::try_from(entries.len())
            .map_err(|_| DiagnosticsExportError::InvalidSnapshot("too many diagnostics entries"))?;
        let mut output = Vec::new();
        let mut central = Vec::new();

        for entry in entries {
            let name = entry.name.as_bytes();
            let name_length = u16::try_from(name.len()).map_err(|_| {
                DiagnosticsExportError::InvalidSnapshot("diagnostics entry name is too long")
            })?;
            let data_length = u32::try_from(entry.data.len()).map_err(|_| {
                DiagnosticsExportError::EntryTooLarge {
                    name: entry.name.clone(),
                    bytes: entry.data.len(),
                }
            })?;
            let local_offset = u32::try_from(output.len()).map_err(|_| {
                DiagnosticsExportError::ArchiveTooLarge {
                    bytes: output.len(),
                }
            })?;
            let checksum = crc32(&entry.data);

            push_u32(&mut output, 0x0403_4b50);
            push_u16(&mut output, ZIP_VERSION_20);
            push_u16(&mut output, ZIP_UTF8_FLAG);
            push_u16(&mut output, 0);
            push_u16(&mut output, 0);
            push_u16(&mut output, ZIP_DOS_DATE_1980_01_01);
            push_u32(&mut output, checksum);
            push_u32(&mut output, data_length);
            push_u32(&mut output, data_length);
            push_u16(&mut output, name_length);
            push_u16(&mut output, 0);
            output.extend_from_slice(name);
            output.extend_from_slice(&entry.data);

            push_u32(&mut central, 0x0201_4b50);
            push_u16(&mut central, ZIP_VERSION_20);
            push_u16(&mut central, ZIP_VERSION_20);
            push_u16(&mut central, ZIP_UTF8_FLAG);
            push_u16(&mut central, 0);
            push_u16(&mut central, 0);
            push_u16(&mut central, ZIP_DOS_DATE_1980_01_01);
            push_u32(&mut central, checksum);
            push_u32(&mut central, data_length);
            push_u32(&mut central, data_length);
            push_u16(&mut central, name_length);
            push_u16(&mut central, 0);
            push_u16(&mut central, 0);
            push_u16(&mut central, 0);
            push_u16(&mut central, 0);
            push_u32(&mut central, 0);
            push_u32(&mut central, local_offset);
            central.extend_from_slice(name);
        }

        let central_offset =
            u32::try_from(output.len()).map_err(|_| DiagnosticsExportError::ArchiveTooLarge {
                bytes: output.len(),
            })?;
        let central_size =
            u32::try_from(central.len()).map_err(|_| DiagnosticsExportError::ArchiveTooLarge {
                bytes: central.len(),
            })?;
        output.extend_from_slice(&central);
        push_u32(&mut output, 0x0605_4b50);
        push_u16(&mut output, 0);
        push_u16(&mut output, 0);
        push_u16(&mut output, entry_count);
        push_u16(&mut output, entry_count);
        push_u32(&mut output, central_size);
        push_u32(&mut output, central_offset);
        push_u16(&mut output, 0);

        if output.len() > MAX_ARCHIVE_BYTES {
            return Err(DiagnosticsExportError::ArchiveTooLarge {
                bytes: output.len(),
            });
        }
        Ok(output)
    }
}

fn push_u16(target: &mut Vec<u8>, value: u16) {
    target.extend_from_slice(&value.to_le_bytes());
}

fn push_u32(target: &mut Vec<u8>, value: u32) {
    target.extend_from_slice(&value.to_le_bytes());
}

fn crc32(data: &[u8]) -> u32 {
    let mut crc = u32::MAX;
    for byte in data {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb8_8320 & (0_u32.wrapping_sub(crc & 1)));
        }
    }
    !crc
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        DiagnosticError, DiagnosticLogEvent, LogsDiagnostics, SecretKind, SessionDiagnostics,
    };
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    static TEST_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn snapshot() -> DiagnosticSnapshot {
        let mut snapshot = DiagnosticSnapshot::new("1.2.3-test");
        snapshot.generated_at_unix_ms = 42;
        snapshot.build = "Release".to_owned();
        snapshot.commit_sha = "0123456789ab".to_owned();
        snapshot.runtime_mode = "cpp-authority/rust-shadow-off".to_owned();
        snapshot.session = Some(SessionDiagnostics {
            state: "Connected".to_owned(),
            session_generation: 7,
            peer_generation: 9,
            negotiation_generation: 11,
            recovery_generation: 12,
            operation_generation: 13,
            channel_generation: 10,
            recovery_history: Vec::new(),
            connection_timeline: Vec::new(),
        });
        snapshot.logs = LogsDiagnostics {
            recent_errors: vec![DiagnosticLogEvent {
                timestamp_unix_ms: 42,
                component: "auth".to_owned(),
                code: "auth-failed".to_owned(),
                message: "controller_token=controller-sentinel".to_owned(),
            }],
            warnings: Vec::new(),
            recovery_events: Vec::new(),
        };
        snapshot.errors.push(DiagnosticError {
            component: "turn".to_owned(),
            message: "TURN secret: turn-sentinel".to_owned(),
        });
        snapshot
    }

    fn unique_path(name: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock after epoch")
            .as_nanos();
        let sequence = TEST_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        std::env::temp_dir().join(format!(
            "desklink-diagnostics-{}-{nanos}-{sequence}-{name}",
            std::process::id()
        ))
    }

    fn contains(haystack: &[u8], needle: &[u8]) -> bool {
        haystack
            .windows(needle.len())
            .any(|window| window == needle)
    }

    #[test]
    fn exports_fixed_local_zip_entries_after_redaction() {
        let output = unique_path("bundle.zip");
        let redactor = DiagnosticsRedactor::new()
            .with_secret(SecretKind::ControllerToken, "controller-sentinel")
            .with_secret(SecretKind::TurnSecret, "turn-sentinel");

        DiagnosticsExporter::export_with_redactor(&snapshot(), &output, &redactor)
            .expect("export diagnostics bundle");
        let archive = fs::read(&output).expect("read diagnostics bundle");
        fs::remove_file(&output).expect("remove diagnostics bundle");

        assert!(archive.starts_with(&0x0403_4b50_u32.to_le_bytes()));
        for name in [
            "manifest.json",
            "system.json",
            "desklink.json",
            "session.json",
            "network.json",
            "media.json",
            "service.json",
            "logs.json",
        ] {
            assert!(contains(&archive, name.as_bytes()), "missing {name}");
        }
        assert!(!contains(&archive, b"controller-sentinel"));
        assert!(!contains(&archive, b"turn-sentinel"));
        assert!(contains(&archive, crate::REDACTED.as_bytes()));
        assert!(contains(&archive, b"\"auto_upload\": false"));
    }

    #[test]
    fn session_is_optional_for_one_shot_collection() {
        let output = unique_path("idle.zip");
        let mut snapshot = DiagnosticSnapshot::new("1.2.3-test");
        snapshot.session = None;

        DiagnosticsExporter::export_to_path(&snapshot, &output).expect("export idle snapshot");
        let archive = fs::read(&output).expect("read idle snapshot");
        fs::remove_file(&output).expect("remove idle snapshot");

        assert!(contains(&archive, b"session.json"));
        assert!(contains(&archive, b"null"));
    }

    #[test]
    fn never_overwrites_an_existing_bundle() {
        let output = unique_path("existing.zip");
        fs::write(&output, b"keep-me").expect("seed output");

        let error = DiagnosticsExporter::export_to_path(&snapshot(), &output)
            .expect_err("existing bundle must not be overwritten");
        assert!(matches!(error, DiagnosticsExportError::Io(_)));
        assert_eq!(fs::read(&output).expect("read existing output"), b"keep-me");
        fs::remove_file(&output).expect("remove existing output");
    }

    #[test]
    fn invalid_snapshot_does_not_create_a_file() {
        let output = unique_path("invalid.zip");
        let mut snapshot = snapshot();
        snapshot.schema_version += 1;

        assert!(matches!(
            DiagnosticsExporter::export_to_path(&snapshot, &output),
            Err(DiagnosticsExportError::InvalidSnapshot(_))
        ));
        assert!(!output.exists());
    }
}
