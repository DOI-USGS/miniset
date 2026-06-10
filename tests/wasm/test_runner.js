/**
 * Simple test runner for miniset WASM tests
 */

class TestRunner {
    constructor(name) {
        this.suiteName = name;
        this.tests = [];
        this.passed = 0;
        this.failed = 0;
        this.skipped = 0;
    }

    test(name, fn) {
        this.tests.push({ name, fn });
    }

    async run() {
        console.log(`\n${'='.repeat(60)}`);
        console.log(`  ${this.suiteName}`);
        console.log(`${'='.repeat(60)}\n`);

        for (const { name, fn } of this.tests) {
            try {
                await fn();
                this.passed++;
                console.log(`  ✓ ${name}`);
            } catch (error) {
                this.failed++;
                console.log(`  ✗ ${name}`);
                console.log(`    Error: ${error.message}`);
                if (error.stack) {
                    const stackLines = error.stack.split('\n').slice(1, 4);
                    stackLines.forEach(line => console.log(`    ${line.trim()}`));
                }
            }
        }

        console.log(`\n${'─'.repeat(60)}`);
        console.log(`  Results: ${this.passed} passed, ${this.failed} failed, ${this.skipped} skipped`);
        console.log(`${'='.repeat(60)}\n`);

        return this.failed === 0;
    }
}

function assert(condition, message) {
    if (!condition) {
        throw new Error(message || 'Assertion failed');
    }
}

function assertEqual(actual, expected, message) {
    if (actual !== expected) {
        throw new Error(
            message || `Expected ${expected}, but got ${actual}`
        );
    }
}

function assertClose(actual, expected, tolerance, message) {
    const diff = Math.abs(actual - expected);
    if (diff > tolerance) {
        throw new Error(
            message || `Expected ${expected} ± ${tolerance}, but got ${actual} (diff: ${diff})`
        );
    }
}

function assertThrows(fn, message) {
    let threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
    }
    if (!threw) {
        throw new Error(message || 'Expected function to throw, but it did not');
    }
}

export { TestRunner, assert, assertEqual, assertClose, assertThrows };
