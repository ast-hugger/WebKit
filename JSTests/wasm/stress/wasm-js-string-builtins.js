import * as assert from '../assert.js';
import { watToBuffer } from "../wabt-wrapper.js";

// Exercise all available pathways for instantiation with compileOptions introduced by the proposal
async function testInstantiation() {
    const wat = `
    (module
        (import "wasm:js-string" "concat" (func $concatBuiltin (param externref externref) (result externref)))
        (func (export "foo") (result i32)
            i32.const 42
        )
    )`;
    const buffer = await watToBuffer(wat);

    // bytes -> module -> instance
    const module1 = new WebAssembly.Module(buffer, { builtins: ['js-string'] });
    const instance1 = new WebAssembly.Instance(module1, {});
    assert.isNotUndef(instance1.exports.foo);

    // bytes --async WA--> module -> instance
    const module2 = await WebAssembly.compile(buffer, { builtins: ['js-string'] });
    const instance2 = new WebAssembly.Instance(module2, {});
    assert.isNotUndef(instance2.exports.foo);

    // bytes --async WA--> instance
    const instantiatedSource = await WebAssembly.instantiate(buffer, {}, { builtins: ['js-string'] });
    const instance3 = instantiatedSource.instance;
    assert.isNotUndef(instance3.exports.foo);
}

// For the tests that follow
async function instantiate(wat) {
    const buffer = await watToBuffer(wat);
    const result = await WebAssembly.instantiate(buffer, {}, { builtins: ['js-string'] });
    return result.instance;
}

async function testCast() {
    const wat = `
    (module
        (import "wasm:js-string" "cast" (func $builtin (param externref) (result externref)))
        (export "exported" (func $builtin))
        (func (export "wrapper") (param $arg externref) (result externref)
            local.get $arg
            call $builtin
        )
    )`;
    const instance = await instantiate(wat);
    const wrapped = instance.exports.wrapper;
    assert.eq("foobar", wrapped("foobar"));
    assert.eq("", wrapped(""));
    // try {
        wrapped(42);
    // } catch (exception) {
    // }
    // const exported = instance.exports.exported;
    // assert.eq("foobar", exported("foobar"));
    // assert.eq("", exported(""));
    // try {
    //     exported(42);
    // } catch (exception) {
    // }
}

async function testTest() {
    const wat = `
    (module
        (import "wasm:js-string" "test" (func $builtin (param externref) (result i32)))
        (export "reExported" (func $builtin))
        (func (export "wrapper") (param $arg externref) (result i32)
            local.get $arg
            call $builtin
        )
    )`;
    const instance = await instantiate(wat);
    const wrapped = instance.exports.wrapper;
    assert.eq(1, wrapped("foobar"));
    assert.eq(1, wrapped(""));
    assert.eq(0, wrapped(42));
    const reExported = instance.exports.reExported;
    assert.eq(1, reExported("foobar"));
    assert.eq(1, reExported(""));
    assert.eq(0, reExported(42));
}

async function testConcat() {
    const wat = `
    (module
        (import "wasm:js-string" "concat" (func $concatBuiltin (param externref externref) (result externref)))
        (func (export "concatWrapper") (param $left externref) (param $right externref) (result externref)
            local.get $left
            local.get $right
            call $concatBuiltin
        )
    )`;
    const instance = await instantiate(wat);
    const callConcat = instance.exports.concatWrapper;
    assert.eq("foobar", callConcat("foo", "bar"));
    assert.eq("foobar", callConcat("", "foobar"));
    assert.eq("foobar", callConcat("foobar", ""));
}

await assert.asyncTest(testInstantiation());
await assert.asyncTest(testCast());
await assert.asyncTest(testTest());
await assert.asyncTest(testConcat());
