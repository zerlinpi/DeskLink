//! Shadow-mode telemetry primitives.
//!
//! This module is intentionally passive: telemetry records comparisons only.
//! It must never influence production authority decisions.

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ShadowEventKind {
    StateTransition,
    GenerationMismatch,
    RecoveryDivergence,
    ChannelOrdering,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ShadowComparison {
    pub event_kind: ShadowEventKind,
    pub cpp_state: u8,
    pub rust_state: u8,
    pub mismatch: bool,
}

#[derive(Debug, Default)]
pub struct ShadowTelemetryRecorder {
    mismatch_count: u64,
    event_count: u64,
}

impl ShadowTelemetryRecorder {
    pub const fn new() -> Self {
        Self {
            mismatch_count: 0,
            event_count: 0,
        }
    }

    pub fn record(&mut self, comparison: ShadowComparison) {
        self.event_count += 1;
        if comparison.mismatch {
            self.mismatch_count += 1;
        }
    }

    pub const fn events(&self) -> u64 {
        self.event_count
    }

    pub const fn mismatches(&self) -> u64 {
        self.mismatch_count
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn telemetry_does_not_change_authority() {
        let mut recorder = ShadowTelemetryRecorder::new();
        recorder.record(ShadowComparison {
            event_kind: ShadowEventKind::GenerationMismatch,
            cpp_state: 1,
            rust_state: 2,
            mismatch: true,
        });

        assert_eq!(recorder.events(), 1);
        assert_eq!(recorder.mismatches(), 1);
    }
}
