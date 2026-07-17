// RUN: %clang-refold-tester macro_shared_frontier_pure_insertion_refold
// 1. Define the "Child" macros that contain the logic
#define ACTION_A(x) ((x) * 2)
#define ACTION_B(x) ((x) + 10)

// 2. The Dispatcher: This forces one extra level of evaluation
#define EVAL(x) x
#define DISPATCH(macro_name, value) EVAL(macro_name(value))

// 3. The "Parent" macro: Orchestrates multiple operations
#define NESTED_OP(op1, op2, val) \
    DISPATCH(op2, DISPATCH(op1, val))

int main() {
    // This performs: ACTION_B(ACTION_A(5))
    // (5 * 2) = 10 -> (10 + 10) = 20
    int result = NESTED_OP(ACTION_A, ACTION_B, (7) * 5);
    
    printf("Result: %d\n", result); // Outputs 20
    return 0;
}
