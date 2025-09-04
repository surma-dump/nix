#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/util/error.hh"
#include "nix/util/strings.hh"
#include "nix/util/canon-path.hh"

#include <wasm_c_api.h>
#include <wasm_export.h>

namespace nix {

static void prim_loadWasm(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    NixStringContext context;
    auto path = state.coerceToPath(pos, *args[0], context, "while evaluating the path passed to builtins.loadWasm");
    auto funcName = state.forceStringNoCtx(*args[1], pos, "while evaluating the function name passed to builtins.loadWasm");
    
    // Realize any context if necessary
    if (!context.empty() && path.accessor == state.rootFS) {
        auto rewrites = state.realiseContext(context);
        path = {path.accessor, CanonPath(rewriteStrings(path.path.abs(), rewrites))};
    }
    
    auto wasmBytes = path.readFile();

    char error_buf[128];
    wasm_module_t module;
    wasm_module_inst_t module_inst;
    wasm_function_inst_t func;
    wasm_exec_env_t exec_env;
    uint32_t stack_size = 8092, heap_size = 8092;

    if (!wasm_runtime_init()) {
        state.error<EvalError>("failed to initialize wasm runtime").atPos(pos).debugThrow();
    }

    module = wasm_runtime_load((uint8_t*)wasmBytes.data(), wasmBytes.size(), error_buf, sizeof(error_buf));
    if (!module) {
        wasm_runtime_destroy();
        state.error<EvalError>("failed to load wasm module: %s", error_buf).atPos(pos).debugThrow();
    }

    module_inst = wasm_runtime_instantiate(module, stack_size, heap_size, error_buf, sizeof(error_buf));
    if (!module_inst) {
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("failed to instantiate wasm module: %s", error_buf).atPos(pos).debugThrow();
    }

    exec_env = wasm_runtime_create_exec_env(module_inst, stack_size);
    if (!exec_env) {
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("failed to create wasm execution environment").atPos(pos).debugThrow();
    }

    func = wasm_runtime_lookup_function(module_inst, std::string(funcName).c_str());
    
    if (!func) {
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("failed to find function '%s' in wasm module", funcName).atPos(pos).debugThrow();
    }

    uint32_t argv[1] = { 0 };
    if (!wasm_runtime_call_wasm(exec_env, func, 1, argv)) {
        std::string exception(wasm_runtime_get_exception(module_inst));
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("failed to call wasm function: %s", exception).atPos(pos).debugThrow();
    }

    // Get the return value from argv[0]
    int32_t result = (int32_t)argv[0];

    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
    wasm_runtime_destroy();

    v.mkInt(result);
}

static RegisterPrimOp primop_loadWasm({
    .name = "loadWasm",
    .args = {"path", "functionName"},
    .doc = R"(
      Load a WebAssembly module from the given path, and execute the specified function.
      Returns the integer value returned by the function.
      
      Example: builtins.loadWasm ./module.wasm "add"
    )",
    .fun = prim_loadWasm,
});

}