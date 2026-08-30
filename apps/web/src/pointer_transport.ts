export const POINTER_MOVE_BUFFER_BUDGET_BYTES = 16;

export function shouldDeferPointerMove(
  bufferedAmount: number,
  budgetBytes = POINTER_MOVE_BUFFER_BUDGET_BYTES,
): boolean {
  if (!Number.isFinite(bufferedAmount)) return true;
  const normalizedAmount = Math.max(0, bufferedAmount);
  const normalizedBudget = Number.isFinite(budgetBytes)
    ? Math.max(0, budgetBytes)
    : POINTER_MOVE_BUFFER_BUDGET_BYTES;
  return normalizedAmount > normalizedBudget;
}
