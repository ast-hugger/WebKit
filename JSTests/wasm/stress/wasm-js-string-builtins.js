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
    const wrapper = instance.exports.wrapper;
    const exported = instance.exports.exported;

    assert.eq("foobar", wrapper("foobar"));
    assert.eq("", wrapper(""));
    assert.throwsAny(wrapper, 42);

    assert.eq("foobar", exported("foobar"));
    assert.eq("", exported(""));
    assert.throwsAny(exported, 42);
}

async function testTest() {
    const wat = `
    (module
        (import "wasm:js-string" "test" (func $builtin (param externref) (result i32)))
        (export "exported" (func $builtin))
        (func (export "wrapper") (param $arg externref) (result i32)
            local.get $arg
            call $builtin
        )
    )`;
    const instance = await instantiate(wat);
    const wrapper = instance.exports.wrapper;
    const exported = instance.exports.exported;

    assert.eq(1, wrapper("foobar"));
    assert.eq(1, wrapper(""));
    assert.eq(0, wrapper(42));

    assert.eq(1, exported("foobar"));
    assert.eq(1, exported(""));
    assert.eq(0, exported(42));
}

async function testConcat() {
    const wat = `
    (module
        (import "wasm:js-string" "concat" (func $builtin (param externref externref) (result externref)))
        (export "exported" (func $builtin))
        (func (export "wrapper") (param $left externref) (param $right externref) (result externref)
            local.get $left
            local.get $right
            call $builtin
        )
    )`;
    const instance = await instantiate(wat);
    const wrapper = instance.exports.wrapper;
    const exported = instance.exports.exported;

    assert.eq("foobar", wrapper("foo", "bar"));
    assert.eq("foobar", wrapper("", "foobar"));
    assert.eq("foobar", wrapper("foobar", ""));
    assert.throwsAny(wrapper, "foo", 42);
    assert.throwsAny(wrapper, 42, "foo");

    assert.eq("foobar", exported("foo", "bar"));
    assert.eq("foobar", exported("", "foobar"));
    assert.eq("foobar", exported("foobar", ""));
    assert.throwsAny(exported, "foo", 42);
    assert.throwsAny(exported, 42, "foo");
}

await assert.asyncTest(testInstantiation());
await assert.asyncTest(testCast());
await assert.asyncTest(testTest());
await assert.asyncTest(testConcat());
