use crate::ChaosScenario;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ChaosOutcome {
    Passed,
    Failed,
    NotExecuted,
}

#[derive(Debug, Clone)]
pub struct ChaosResult {
    pub scenario: ChaosScenario,
    pub outcome: ChaosOutcome,
    pub stale_events: u64,
    pub recovery_attempts: u32,
}

pub trait FaultInjector {
    fn inject(&mut self, scenario: &ChaosScenario);
}

pub struct ChaosRunner<I: FaultInjector> {
    injector: I,
}

impl<I: FaultInjector> ChaosRunner<I> {
    pub fn new(injector: I) -> Self {
        Self { injector }
    }

    pub fn run(&mut self, scenario: ChaosScenario) -> ChaosResult {
        self.injector.inject(&scenario);

        ChaosResult {
            scenario,
            outcome: ChaosOutcome::NotExecuted,
            stale_events: 0,
            recovery_attempts: 0,
        }
    }
}
