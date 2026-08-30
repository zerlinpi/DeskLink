export type AsyncAttemptId = symbol;

export class AsyncAttemptCoordinator {
  private active: AsyncAttemptId | null = null;

  begin(label = "attempt"): AsyncAttemptId | null {
    if (this.active) return null;
    const id = Symbol(label);
    this.active = id;
    return id;
  }

  isCurrent(id: AsyncAttemptId): boolean {
    return this.active === id;
  }

  invalidate(): void {
    this.active = null;
  }

  finish(id: AsyncAttemptId): void {
    if (this.active === id) this.active = null;
  }
}
