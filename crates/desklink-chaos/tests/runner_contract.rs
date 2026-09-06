use desklink_chaos::{
    ChaosEvaluation, ChaosEvaluator, ChaosOutcome, ChaosRunner, ChaosScenario, FaultInjector,
};

#[derive(Default)]
struct RecordingInjector {
    calls: Vec<ChaosScenario>,
}

impl FaultInjector for RecordingInjector {
    fn inject(&mut self, scenario: &ChaosScenario) {
        self.calls.push(*scenario);
    }
}

#[derive(Default)]
struct RecordingEvaluator {
    calls: Vec<ChaosScenario>,
}

impl ChaosEvaluator for RecordingEvaluator {
    fn evaluate(&mut self, scenario: &ChaosScenario) -> ChaosEvaluation {
        self.calls.push(*scenario);
        ChaosEvaluation::recovered(Some("ice-restart".to_string()), 3, 2)
    }
}

#[test]
fn runner_injects_once_and_reports_not_executed_without_observer() {
    let mut runner = ChaosRunner::new(RecordingInjector::default());

    let result = runner.run(ChaosScenario::NetworkDrop);

    assert_eq!(result.scenario, ChaosScenario::NetworkDrop);
    assert_eq!(result.outcome, ChaosOutcome::NotExecuted);
    assert_eq!(result.stale_events, 0);
    assert_eq!(result.recovery_attempts, 0);
    assert_eq!(result.recovery_level, None);
    assert_eq!(runner.injector().calls, vec![ChaosScenario::NetworkDrop]);
}

#[test]
fn runner_uses_evaluator_after_fault_injection() {
    let mut runner = ChaosRunner::with_evaluator(
        RecordingInjector::default(),
        RecordingEvaluator::default(),
    );

    let result = runner.run(ChaosScenario::PeerReplace);

    assert_eq!(result.scenario, ChaosScenario::PeerReplace);
    assert_eq!(result.outcome, ChaosOutcome::Recovered);
    assert_eq!(result.recovery_level.as_deref(), Some("ice-restart"));
    assert_eq!(result.stale_events, 3);
    assert_eq!(result.recovery_attempts, 2);
    assert_eq!(runner.injector().calls, vec![ChaosScenario::PeerReplace]);
    assert_eq!(runner.evaluator().calls, vec![ChaosScenario::PeerReplace]);
}
