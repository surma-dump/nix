{
  callPackage,
	lib,
}:
let
  plugin = callPackage (import ../.) {};
  tests = {
    parseAYaml = {
      expr = ./example.yml |> lib.readFile |> builtins.runWasm "${plugin}/lib/plugin.wasm" "parse_yaml";
      expected = {
        a = 1;
        b = "hi";
        c = [
          1
          2
          {
            x = 1;
            y = 2;
          }
        ];
      };
    };
  };
in
lib.runTests ({
  tests = tests |> lib.attrNames;
} // tests)
