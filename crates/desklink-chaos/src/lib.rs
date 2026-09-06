//! Session chaos testing primitives.
//!
//! This crate only describes fault scenarios. It does not inject failures into
//! production paths. Integration layers decide how scenarios are executed.

mod runner;

pub use runner::{ChaosEvaluator, ChaosRunner, FaultInjector, NoopEvaluator};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ChaosScenario {
    NetworkDrop,
    NetworkRestore,
    TurnSwitch,
    PeerReplace,
    ControllerRefresh,
    BrowserSleep,
    WindowsSleep,
    ServiceRestart,
    GpuReset,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ChaosOutcome {
    Recovered,
    Failed,
    NotExecuted,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ChaosEvaluation {
    pub outcome: ChaosOutcome,
    pub recovery_level: Option<String>,
    pub stale_events: u64,
    pub recovery_attempts: u32,
}

impl ChaosEvaluation {
    pub fn recovered(
        recovery_level: Option<String>,
        stale_events: u64,
        recovery_attempts: u32,
    ) -> Self {
        Self {
            outcome: ChaosOutcome::Recovered,
            recovery_level,
            stale_events,
            recovery_attempts,
        }
    }

    pub fn failed(
        recovery_level: Option<String>,
        stale_events: u64,
        recovery_attempts: u32,
    ) -> Self {
        Self {
            outcome: ChaosOutcome::Failed,
            recovery_level,
            stale_events,
            recovery_attempts,
        }
    }

    pub const fn not_executed() -> Self {
        Self {
            outcome: ChaosOutcome::NotExecuted,
            recovery_level: None,
            stale_events: 0,
            recovery_attempts: 0,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ChaosResult {
    pub scenario: ChaosScenario,
    pub outcome: ChaosOutcome,
    pub recovery_level: Option<String>,
    pub stale_events: u64,
    pub recovery_attempts: u32,
}

impl ChaosResult {
    pub const fn recovered(scenario: ChaosScenario, stale_events: u64) -> Self {
        Self {
            scenario,
            outcome: ChaosOutcome::Recovered,
            recovery_level: None,
            stale_events,
            recovery_attempts: 0,
        }
    }

    pub const fn not_executed(scenario: ChaosScenario) -> Self {
        Self {
            scenario,
            outcome: ChaosOutcome::NotExecuted,
            recovery_level: None,
            stale_events: 0,
            recovery_attempts: 0,
        }
    }

    pub fn from_evaluation(scenario: ChaosScenario, evaluation: ChaosEvaluation) -> Self {
        Self {
            scenario,
            outcome: evaluation.outcome,
            recovery_level: evaluation.recovery_level,
            stale_events: evaluation.stale_events,
            recovery_attempts: evaluation.recovery_attempts,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chaos_scenario_catalog_is_stable() {
        assert_eq!(ChaosScenario::NetworkDrop, ChaosScenario::NetworkDrop);
    }
}
