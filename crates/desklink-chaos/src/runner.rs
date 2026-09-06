use crate::{ChaosEvaluation, ChaosResult, ChaosScenario};

pub trait FaultInjector {
    fn inject(&mut self, scenario: &ChaosScenario);
}

pub trait ChaosEvaluator {
    fn evaluate(&mut self, scenario: &ChaosScenario) -> ChaosEvaluation;
}

#[derive(Default)]
pub struct NoopEvaluator;

impl ChaosEvaluator for NoopEvaluator {
    fn evaluate(&mut self, _scenario: &ChaosScenario) -> ChaosEvaluation {
        ChaosEvaluation::not_executed()
    }
}

pub struct ChaosRunner<I: FaultInjector, E: ChaosEvaluator = NoopEvaluator> {
    injector: I,
    evaluator: E,
}

impl<I: FaultInjector> ChaosRunner<I, NoopEvaluator> {
    pub fn new(injector: I) -> Self {
        Self {
            injector,
            evaluator: NoopEvaluator,
        }
    }
}

impl<I: FaultInjector, E: ChaosEvaluator> ChaosRunner<I, E> {
    pub fn with_evaluator(injector: I, evaluator: E) -> Self {
        Self {
            injector,
            evaluator,
        }
    }

    pub fn injector(&self) -> &I {
        &self.injector
    }

    pub fn evaluator(&self) -> &E {
        &self.evaluator
    }

    pub fn run(&mut self, scenario: ChaosScenario) -> ChaosResult {
        self.injector.inject(&scenario);
        let evaluation = self.evaluator.evaluate(&scenario);
        ChaosResult::from_evaluation(scenario, evaluation)
    }
}
