use desklink_chaos::{ChaosOutcome, ChaosRunner, ChaosScenario, FaultInjector};

#[derive(Default)]
struct RecordingInjector {
    calls: Vec<ChaosScenario>,
}

impl FaultInjector for RecordingInjector {
    fn inject(&mut self, scenario: &ChaosScenario) {
        self.calls.push(*scenario);
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
