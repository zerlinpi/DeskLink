//! Session chaos testing primitives.
//!
//! This crate only describes fault scenarios. It does not inject failures into
//! production paths. Integration layers decide how scenarios are executed.

mod runner;

pub use runner::{ChaosRunner, FaultInjector};

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
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chaos_scenario_catalog_is_stable() {
        assert_eq!(ChaosScenario::NetworkDrop, ChaosScenario::NetworkDrop);
    }
}
