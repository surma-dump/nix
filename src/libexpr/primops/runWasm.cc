#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/util/error.hh"
#include "nix/util/strings.hh"
#include "nix/util/canon-path.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/json-to-value.hh"

#include <wasm_c_api.h>
#include <wasm_export.h>
#include <cstring>

namespace nix {

static void prim_runWasm(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    NixStringContext context;
    auto path = state.coerceToPath(pos, *args[0], context, "while evaluating the path passed to builtins.runWasm");
    auto funcName = state.forceStringNoCtx(*args[1], pos, "while evaluating the function name passed to builtins.runWasm");
    
    // Convert the input value to JSON
    std::ostringstream jsonStream;
    NixStringContext jsonContext;
    state.forceValue(*args[2], pos);
    printValueAsJSON(state, true, *args[2], pos, jsonStream, jsonContext, false);
    std::string jsonInput = jsonStream.str();
    
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
    // Increase heap size to accommodate JSON strings
    uint32_t stack_size = 8192, heap_size = 1024 * 1024; // 1MB heap

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

    // Look up the _malloc function
    wasm_function_inst_t malloc_func = wasm_runtime_lookup_function(module_inst, "_malloc");
    if (!malloc_func) {
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("failed to find '_malloc' function in wasm module").atPos(pos).debugThrow();
    }

    // Allocate memory in WASM for the JSON string
    uint32_t json_len = jsonInput.length() + 1; // +1 for null terminator
    uint32_t malloc_argv[1] = { json_len };
    if (!wasm_runtime_call_wasm(exec_env, malloc_func, 1, malloc_argv)) {
        std::string exception(wasm_runtime_get_exception(module_inst));
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("failed to call _malloc: %s", exception).atPos(pos).debugThrow();
    }
    
    uint32_t json_ptr = malloc_argv[0];
    if (!json_ptr) {
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("_malloc returned null pointer").atPos(pos).debugThrow();
    }

    // Copy JSON string to WASM memory
    void* json_addr = wasm_runtime_addr_app_to_native(module_inst, json_ptr);
    if (!json_addr) {
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("failed to convert WASM pointer to native address").atPos(pos).debugThrow();
    }
    memcpy(json_addr, jsonInput.c_str(), json_len);

    // Call the function with the pointer to the JSON string
    uint32_t argv[1] = { json_ptr };
    if (!wasm_runtime_call_wasm(exec_env, func, 1, argv)) {
        std::string exception(wasm_runtime_get_exception(module_inst));
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("failed to call wasm function: %s", exception).atPos(pos).debugThrow();
    }

    // The function returns a pointer to a new null-terminated JSON string
    uint32_t result_ptr = argv[0];
    if (!result_ptr) {
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("wasm function returned null pointer").atPos(pos).debugThrow();
    }
    
    void* result_addr = wasm_runtime_addr_app_to_native(module_inst, result_ptr);
    if (!result_addr) {
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        state.error<EvalError>("failed to convert result pointer to native address").atPos(pos).debugThrow();
    }
    
    // Read the null-terminated result string
    std::string jsonOutput((char*)result_addr);

    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
    wasm_runtime_destroy();

    // Parse the JSON result back into a Nix value
    try {
        parseJSON(state, jsonOutput, v);
    } catch (JSONParseError & e) {
        state.error<EvalError>("failed to parse JSON output from wasm function: %s", e.what()).atPos(pos).debugThrow();
    }
}

static RegisterPrimOp primop_runWasm({
    .name = "runWasm",
    .args = {"path", "functionName", "input"},
    .doc = R"(
      Load a WebAssembly module from the given path, and execute the specified function
      with the given input value converted to JSON.
      
      The WASM module must export a '_malloc' function that takes a size parameter and
      returns a pointer to allocated memory. The input value is converted to JSON and
      copied into WASM memory allocated via '_malloc'. The specified function is called
      with a pointer to this JSON string. The function should return a pointer to a new
      null-terminated JSON string, which is parsed back into a Nix value and returned.
      
      Example: builtins.runWasm ./module.wasm "transform" { foo = "bar"; num = 42; }
    )",
    .fun = prim_runWasm,
});

}
