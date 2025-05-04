import * as assert from '../assert.js';
import { watToBuffer } from "../wabt-wrapper.js";

// Exercise all available pathways for instantiation with compileOptions introduced by the proposal
async function testInstantiation() {
    const wat = `
    (module
        (import "wasm:js-string" "concat" (func $concatBuiltin (param externref externref) (result externref)))
        (export "foo" (func $concatBuiltin))
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
    const result = await WebAssembly.instantiate(buffer, {}, {
        builtins: ['js-string'],
        importedStringConstants: "const"
    });
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

    function check(fun) {
        assert.eq("foobar", fun("foobar"));
        assert.eq("", fun(""));
        assert.throwsAny(fun, 42);
        assert.throwsAny(fun, null);
    }

    check(instance.exports.wrapper);
    check(instance.exports.exported);
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

    function check(fun) {
        assert.eq(1, fun("foobar"));
        assert.eq(1, fun(""));
        assert.eq(0, fun(42));
        assert.eq(0, fun(null));
    }

    check(instance.exports.wrapper);
    check(instance.exports.exported);
}

async function testCharCodeAt() {
    const wat = `
    (module
        (import "wasm:js-string" "charCodeAt" (func $builtin (param externref i32) (result i32)))
        (import "wasm:js-string" "concat" (func $concat (param externref externref) (result externref)))
        (export "exported" (func $builtin))
        (func (export "wrapper") (param $arg externref) (param $len i32) (result i32)
            local.get $arg
            local.get $len
            call $builtin
        )
        (func (export "concat") (param $left externref) (param $right externref) (result externref)
            local.get $left
            local.get $right
            call $concat
        )
    )`;
    const instance = await instantiate(wat);
    const concat = instance.exports.concat;

    const string = "ab😀c";
    const string2 = concat("a😀", "β😦"); // makes a rope
    function check(fun) {
        assert.eq(97, fun(string, 0));
        assert.eq(98, fun(string, 1));
        assert.eq(0xD83D, fun(string, 2));
        assert.eq(0xDE00, fun(string, 3));
        assert.eq(99, fun(string, 4));
        assert.throwsAny(fun, string, 5);
        assert.throwsAny(fun, string, -1);
        assert.throwsAny(fun, 42, 0);

        assert.eq(97, fun(string2, 0));
        assert.eq(0xD83D, fun(string2, 1));
        assert.eq(0xDE00, fun(string2, 2));
        assert.eq(0x3B2, fun(string2, 3));
        assert.eq(0xD83D, fun(string2, 4));
        assert.eq(0xDE26, fun(string2, 5));
        assert.throwsAny(fun, string, 6);
    }

    check(instance.exports.wrapper);
    check(instance.exports.exported);
}

async function testCodePointAt() {
    const wat = `
    (module
        (import "wasm:js-string" "codePointAt" (func $builtin (param externref i32) (result i32)))
        (import "wasm:js-string" "concat" (func $concat (param externref externref) (result externref)))
        (export "exported" (func $builtin))
        (func (export "wrapper") (param $arg externref) (param $len i32) (result i32)
            local.get $arg
            local.get $len
            call $builtin
        )
        (func (export "concat") (param $left externref) (param $right externref) (result externref)
            local.get $left
            local.get $right
            call $concat
        )
    )`;
    const instance = await instantiate(wat);
    const concat = instance.exports.concat;

    const string = 'a😀bΩ';
    const string2 = concat("a😀", "β😦"); // makes a rope
    function check(fun) {
        assert.eq(97, fun(string, 0));
        assert.eq(0x1F600, fun(string, 1));
        assert.eq(0xDE00, fun(string, 2)); // low surrogate of 😀
        assert.eq(98, fun(string, 3));
        assert.eq(937, fun(string, 4));
        assert.throwsAny(fun, string, 5);
        assert.throwsAny(fun, string, -1);
        assert.throwsAny(fun, 42, 0);

        assert.eq(97, fun(string2, 0));
        assert.eq(0x1F600, fun(string2, 1));
        assert.eq(0xDE00, fun(string2, 2));
        assert.eq(0x3B2, fun(string2, 3));
        assert.eq(0x1F626, fun(string2, 4));
        assert.eq(0xDE26, fun(string2, 5)); // low surrogate of 😦
        assert.throwsAny(fun, string, 6);
    }

    check(instance.exports.wrapper);
    check(instance.exports.exported);
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

    function check(fun) {
        assert.eq("foobar", fun("foo", "bar"));
        assert.eq("foobar", fun("", "foobar"));
        assert.eq("foobar", fun("foobar", ""));
        assert.throwsAny(fun, "foo", 42);
        assert.throwsAny(fun, 42, "foo");
    }

    check(instance.exports.wrapper);
    check(instance.exports.exported);
}

async function testLength() {
    const wat = `
    (module
        (import "wasm:js-string" "length" (func $builtin (param externref) (result i32)))
        (export "exported" (func $builtin))
        (func (export "wrapper") (param $arg externref) (result i32)
            local.get $arg
            call $builtin
        )
    )`;
    const instance = await instantiate(wat);

    function check(fun) {
        assert.eq(6, fun("foobar"));
        assert.eq(0, fun(""));
        assert.throwsAny(fun, 42);
        assert.throwsAny(fun, null);
    }

    check(instance.exports.wrapper);
    check(instance.exports.exported);
}

async function testSubstring() {
    const wat = `
    (module
        (import "wasm:js-string" "substring" (func $builtin (param externref i32 i32) (result externref)))
        (import "wasm:js-string" "concat" (func $concat (param externref externref) (result externref)))
        (export "exported" (func $builtin))
        (func (export "wrapper") (param $string externref) (param $start i32) (param $end i32) (result externref)
            local.get $string
            local.get $start
            local.get $end
            call $builtin
        )
        (func (export "concat") (param $left externref) (param $right externref) (result externref)
            local.get $left
            local.get $right
            call $concat
        )
    )`;
    const instance = await instantiate(wat);
    const concat = instance.exports.concat;

    const string = "Hello, world";
    const string2 = concat("Hello", ", world");
    function check(fun) {
        // Normal cases
        assert.eq("lo, w", fun(string, 3, 8));
        assert.eq("lo, w", fun(string2, 3, 8));
        assert.eq("", fun(string, 3, 3));
        assert.eq("", fun(string2, 3, 3));
        assert.eq("", fun(string, 0, 0));
        assert.eq("", fun(string2, 0, 0));
        assert.eq("d", fun(string, 11, 12));
        assert.eq("d", fun(string2, 11, 12));
        assert.eq("d", fun(string, 11, 13));
        assert.eq("d", fun(string2, 11, 13));
        assert.eq("", fun(string, 12, 12));
        assert.eq("", fun(string2, 12, 12));
        // Indices outside bounds
        assert.eq("He", fun(string, -2, 2));
        assert.eq("He", fun(string2, -2, 2));
        assert.eq("orld", fun(string, 8, 20));
        assert.eq("orld", fun(string2, 8, 20));
        assert.eq("Hello, world", fun(string, -100, 100));
        assert.eq("Hello, world", fun(string2, -100, 100));
        // start > end and start > length
        assert.eq("", fun(string, 3, 2));
        assert.eq("", fun(string, 20, 25));
        assert.eq("", fun(string, -3, -2));
        assert.eq("", fun(string, -2, -3));
        assert.eq("", fun(string2, 3, 2));
        assert.eq("", fun(string2, 20, 25));
        assert.eq("", fun(string2, -3, -2));
        assert.eq("", fun(string2, -2, -3));
        // Unicode ouside MBP
        assert.eq("😀", fun("a😀", 1, 3));
    }

    check(instance.exports.wrapper);
    check(instance.exports.exported);
}

async function testEquals() {
    const wat = `
    (module
        (import "wasm:js-string" "equals" (func $builtin (param externref externref) (result i32)))
        (export "exported" (func $builtin))
        (func (export "wrapper") (param $left externref) (param $right externref) (result i32)
            local.get $left
            local.get $right
            call $builtin
        )
    )`;
    const instance = await instantiate(wat);

    function check(fun) {
        assert.eq(1, fun("foo", "foo"));
        assert.eq(1, fun("bar", "bar"));
        assert.eq(0, fun("foo", "bar"));
        assert.eq(0, fun("bar", "foo"));
        assert.eq(1, fun(null, null));
        assert.throwsAny(fun, null, "foo");
        assert.throwsAny(fun, "foo", null);
        assert.throwsAny(fun, "foo", 42);
        assert.throwsAny(fun, 42, "foo");
    }

    check(instance.exports.wrapper);
    check(instance.exports.exported);
}

async function testCompare() {
    const wat = `
    (module
        (import "wasm:js-string" "compare" (func $builtin (param externref externref) (result i32)))
        (export "exported" (func $builtin))
        (func (export "wrapper") (param $left externref) (param $right externref) (result i32)
            local.get $left
            local.get $right
            call $builtin
        )
    )`;
    const instance = await instantiate(wat);

    function check(fun) {
        assert.eq(1, fun("foo", "bar"));
        assert.eq(0, fun("foo", "foo"));
        assert.eq(-1, fun("bar", "foo"));
    }

    check(instance.exports.wrapper);
    check(instance.exports.exported);
}

async function testImportedStringConstants() {
    const wat = `
    (module
        (import "const" "this is constant 1" (global $const1 externref))
        (import "const" "this is constant 2" (global $const2 externref))
        (export "exportedConst2" (global $const2))
        (func (export "returnConst1") (result externref)
            global.get $const1
        )
    )`;
    const instance = await instantiate(wat);

    assert.eq("this is constant 1", instance.exports.returnConst1());
    assert.eq("this is constant 2", instance.exports.exportedConst2.value);
}

await assert.asyncTest(testInstantiation());
await assert.asyncTest(testCast());
await assert.asyncTest(testTest());
await assert.asyncTest(testCharCodeAt());
await assert.asyncTest(testCodePointAt());
await assert.asyncTest(testLength());
await assert.asyncTest(testConcat());
// await assert.asyncTest(testSubstring());
await assert.asyncTest(testEquals());
await assert.asyncTest(testCompare());
await assert.asyncTest(testImportedStringConstants());
