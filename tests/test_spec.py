'''"
This module tests the webassembly spec. It is mostly glue code for converting the json
file generated by tools/unpack_wasm_testsuite.py and running the associated webassembly
using fnpy.machine.runWasm.
'''

from pdb import set_trace as dbg

import json
import os
from .wadze import parse_module, parse_code
from fnpy.env import Env, createWasmEnv
from fnpy.machine import runWasm
from fnpy.wasm import *

SUITE = 'tools/wasm_testsuite_unpacked'

strTys = {
    'f32': F32,
    'f64': F64,
    'i32': I32,
    'i64': I64,
}

def parseWasm(wasmPath):
    with open(wasmPath, 'rb') as f:
        module = parse_module(f.read())
    module['code'] = [parse_code(c) for c in module['code']]
    return module


def runTest(testIndex, env, action, inp, expected):
    assert action['type'] == 'invoke'
    fname = action['field'] # function name to call
    fn = env.fns[env.fnIndexes[fname]]
    for value in inp: env.ds.push(value)
    env.executingFn = fn
    runWasm(env, fn.code)
    env.executingFn = None

    result = []
    expectedTys = [type(v) for v in expected]
    expectedValues = [v.value for v in expected]

    while len(env.ds):
        ty = expectedTys.pop()
        result.append(env.ds.popv(ty))
    assert expectedValues == result, (
        f"\nindex: {testIndex}  inputs: {inp}"
        + f"  expected: {expected}"
        + f"  {fn.debugStr()}"
    )


# From https://github.com/WebAssembly/wabt/blob/main/docs/wast2json.md:
# All numeric value are stored as strings, since JSON numbers are not
# guaranteed to be precise enough to store all Wasm values. Values are always
# written as decimal numbers. For example, the following const has the type
# "i32" and the value 34.
#
# ("type": "i32", "value": "34"}
# 
# For floats, the numbers are written as the decimal encoding of the binary
# representation of the number. For example, the following const has the type
# "f32" and the value 1.0. The value is 1065353216 because that is equivalent
# to hexadecimal 0x3f800000, which is the binary representation of 1.0 as a
# 32-bit float.

# ("type": "f32", "value": "1065353216"}
def _convertJsonValue(json):
    jty, jval = json['type'], int(json['value'])
    if jty.startswith('i'):
        val = strTys[jty](jval)
    elif jty == 'f32':
        jvalBytes = bytes(U32(jval))
        val = F32.from_buffer_copy(jvalBytes)
    elif jty == 'f64':
        jvalBytes = bytes(U64(jval))
        val = F64.from_buffer_copy(jvalBytes)
    else: assert False, 'unknown ' + jty
    return val


# { "type": "assert_return", "line": 1057,
#   "action": {"type": "invoke", "field": "f", "args": []},
#   "expected": [{"type": "f64", "value": "18442240474082181119"}]},
def _assertReturnInOut(action) -> Tuple[List[any], List[any]]:
    """Get the inputs/outputs of an assert return."""
    return inp, out


def convertJsonTys(stys: List[str]) -> List[DataTy]:
    """Convert a list of str tys to a list of DataTys."""
    return [strTys[t] for t in stys]


def convertJsonCode(jsonInstructions: List[Tuple[any]]) -> List[Tuple[any]]:
    fcode = [] # fngi's wasm repr
    for strInstr in jsonInstructions:
        wiStr, *args = strInstr
        fcode.append([wasmCode[wiStr]] + args)
    return fcode


def compileModule(wasmPath: str) -> Env:
    """Compile a web assembly module into an Env instance."""
    wasm = parseWasm(wasmPath)

    # The inputs/outputs are in the "type" (aka function type) block.
    # There are typically not many of these.
    inputs = [convertJsonTys(fty.params) for fty in wasm['type']]
    outputs = [convertJsonTys(fty.returns) for fty in wasm['type']]

    # Locals and code are in the code block
    locals_ = [convertJsonTys(code.locals) for code in wasm['code']]
    code = [convertJsonCode(code.instructions) for code in wasm['code']]

    fnNames = {exportFn.ref: exportFn.name for exportFn in wasm['export']}

    # Clear whatever functions exist in the environment.
    # we are going to replace them.
    env = createWasmEnv()
    for index, tyIndex in enumerate(wasm['func']):
        fnName = fnNames.get(index)
        wasmFn = WasmFn(
            name=fnNames.get(index),
            inputs=inputs[tyIndex],
            outputs=outputs[tyIndex],
            locals_=locals_[index],
            code=code[index],
        )
        env.fns.append(wasmFn)

    env.indexFns()
    return env



def runTests(wasmDir):
    errors = []
    passed = 0
    dirName = os.path.split(wasmDir)[1]
    jsonFile = os.path.join(wasmDir, dirName + '.json')
    with open(jsonFile) as f: j = json.load(f)

    # Note: assert_return does NOT have the module info in it.
    # {"type": "module", "line": 440, "filename": "const.178.wasm"}
    # {"type": "assert_return", "line": 441, "action": {... }, "expected": [...]}
    modulePath = None
    env = None
    for testIndex, test in enumerate(j['commands']):
        testTy = test['type']
        if testTy == 'module':
            # Modules are used to setup a set of future test runs.
            # We must get a new env and "compile" the module so that
            # the assert_return/etc stanzas can be executed.
            modulePath = os.path.join(wasmDir, test['filename'])
            env = compileModule(modulePath)

        elif testTy == 'assert_return':
            action = test['action']
            inp = [_convertJsonValue(j) for j in action['args']]
            expected = [_convertJsonValue(j) for j in test['expected']]

            try:
                runTest(testIndex, env, action, inp, expected)
                passed += 1
            except Exception as e:
                # raise # TODO: remove
                errors.append(f'{modulePath}: {e}')

        elif testTy in {'assert_malformed'}: pass
        elif testTy in {'assert_invalid'}: pass
        elif testTy in {'assert_trap'}:
            pass # TODO: implement traps
        else: errors.append(f'{modulePath}: Unkown testTy {testTy}')

    assert [] == errors, f"Num failed={len(errors)} passed={passed}"


# def test_const_all():
#     runTests('tools/wasm_testsuite_unpacked/const')

# TODO: requires adding functions and parameters to the module
# TODO: requires adding missing opcodes to wadze
def test_i32_all():
    wasmDir = 'tools/wasm_testsuite_unpacked/i32'
    runTests(wasmDir)
    # with open(wasmDir + 'i32.json') as f: jdict = json.load(f)
    # module = compileModule(os.path.join(wasmDir, jdict['commands'][0]['filename']))
    # assert False

    module = parseWasm('tools/wasm_testsuite_unpacked/i32/i32.0.wasm')
    # runTests('tools/wasm_testsuite_unpacked/i32')
