#include <nix/expr/eval.hh>
#include <nix/expr/primops.hh>

namespace nix {

static void prim_add2(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto i1 = state.forceInt(*args[0], pos, "while evaluating the first argument passed to add2");
    auto i2 = state.forceInt(*args[1], pos, "while evaluating the second argument passed to add2");

    auto result_ = i1 + i2;
    if (auto result = result_.valueChecked(); result.has_value()) {
        v.mkInt(*result);
    } else {
        state.error<EvalError>("integer overflow in adding %1% + %2%", i1, i2).atPos(pos).debugThrow();
    }
}

static RegisterPrimOp primop_add2({
    .name = "add2",
    .args = {"x", "y"},
    .doc = R"(
      Return the sum of the integers `x` and `y`.
      This is a simple example of a builtin function.
    )",
    .fun = prim_add2,
});

}
