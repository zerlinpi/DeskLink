use crate::{ChaosResult, ChaosScenario};

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

    pub fn injector(&self) -> &I {
        &self.injector
    }

    pub fn run(&mut self, scenario: ChaosScenario) -> ChaosResult {
        self.injector.inject(&scenario);
        ChaosResult::not_executed(scenario)
    }
}
