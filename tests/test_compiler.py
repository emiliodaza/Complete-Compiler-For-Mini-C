#!/usr/bin/env python3
"""Execute generated i386 machine code and compare with native Clang results."""
import argparse
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from run import checked, clang, execute_object

PREFIX = 'extern void print(int);\nextern int read();\n'
ARGUMENTS = [-7, -1, 0, 1, 2, 6, 11]


def cases(random_cases):
    # Each run is (function argument, read() inputs).
    regular = [(n, []) for n in ARGUMENTS]
    definitions = [
        ('constant', 'return 42;'),
        ('precedence', 'return n + 2 * 3 - 8 / 2;'),
        ('unary', 'return -(-n + 4);'),
        ('signed_division', 'return n / 3;'),
        ('division_negative_divisor', 'return (n * 9 + 2) / -4;'),
        ('same_operand', 'print(n + n); print(n * n); return (n + n) + (n + n);'),
        ('comparison_values', 'print(n < 2); print(n <= 2); print(n > 2); print(n >= 2); print(n == 2); return n != 2;'),
        ('comparison_arithmetic', 'return (n < 5) + 2 * (n >= 0) - (n == 1);'),
        ('comparison_precedence', 'return n == 1 < 2;'),
        ('comparison_nested', 'return (n < 3) == (n > 0);'),
        ('truthy_condition', 'if (n) return 10; else return 20;'),
        ('arithmetic_condition', 'if (n + 1) return n; return 33;'),
        ('constant_branches', 'if (1) print(10); if (0) print(20); return 7;'),
        ('dangling_else', 'if (n > 0) if (n < 4) return 1; else return 2; return 3;'),
        ('branch_join', 'int x; if (n > 0) x = 10; else x = 20; return x + n;'),
        ('same_constant_join', 'int x; if (n) x = 9; else x = 9; return x * 2;'),
        ('changed_store', 'int x; int y; x = n; y = x + 1; x = 40; return y + x;'),
        ('shadowing', 'int x; x = n; { int x; x = 99; print(x); } return x;'),
        ('shadow_parameter', '{ int n; n = 30; print(n); } return n;'),
        ('loop_sum', 'int i; int sum; i = 0; sum = 0; while (i < n) { sum = sum + i; i = i + 1; } return sum;'),
        ('loop_truthiness', 'int i; i = 3; while (i) { print(i); i = i - 1; } return n;'),
        ('nested_loops', 'int i; int j; int s; i = 0; s = 0; while (i < 3) { j = 0; while (j < 2) { s = s + n; j = j + 1; } i = i + 1; } return s;'),
        ('return_in_loop', 'while (n > 0) { if (n == 2) return 22; n = n - 1; } return 0;'),
        ('return_unreachable', 'return n; print(999); return 888;'),
        ('nested_unbraced_if', 'if (n) if (n > 2) { int x; x = 41; return x; } return 5;'),
        ('nested_unbraced_while', 'while (n > 0) if (n > 2) { int x; x = 13; return x; } else n = n - 1; return n;'),
        ('expression_statement', 'n + 1; (n < 3); -n; return n;'),
        ('call_live_values', 'int x; x = n * 3; print(x); print(x + 1); return x - n;'),
        ('pressure', 'return ((n + 1) * (n + 2)) + ((n + 3) * (n + 4)) + ((n + 5) * (n + 6));'),
        ('division_pressure', 'return (n + 20) / 3 + ((n + 40) / 7) * ((n + 10) / 2);'),
        ('constant_division', 'return 100 / 3;'),
        ('large_constant', 'return 2147483647;'),
        ('minimum_integer', 'return -2147483647 - 1;'),
        ('minimum_division', 'return (-2147483647 - 1) / 2;'),
        ('repeated_division', 'int a; int b; a = n + 21; b = n + 30; return (a / 3) * (b / 2) + a / 2;'),
        ('comments', '/* body */ int x; // line comment\nx = n; return x;'),
    ]
    for name, body in definitions:
        yield name, PREFIX + f'int main_function(int n) {{ {body} }}\n', regular
    io_runs = [(n, values) for n in [-2, 0, 6] for values in [[20, 3], [-17, 4], [8, -3]]]
    yield 'read_and_arithmetic', PREFIX + '''int main_function(int n) {
        int a; int b; a = read(); b = read();
        print(a + b); print(a - b); print(a * b); print(a / b);
        return a + b + n;
    }''', io_runs
    yield 'discarded_read', PREFIX + 'int main_function() { read(); return read(); }', [(0, [100, 23])]
    yield 'call_renaming', PREFIX + 'int main_function(int n) { int x; x = n; { int x; x = 7; print(x + read()); } return x; }', [(8, [5])]
    yield 'crlf', (PREFIX + 'int main_function() { return 15; }\n').replace('\n', '\r\n'), [(0, [])]
    # Constants are loaded from read() so optimization cannot remove all spills.
    yield 'many_live_reads', PREFIX + '''int main_function() {
        int a; int b; int c; int d; int e; int f;
        a = read(); b = read(); c = read(); d = read(); e = read(); f = read();
        return (a+b)*(c+d) + (e+f)*(a+c) - (b+d)*(e+a);
    }''', [(0, [2, 3, 4, 5, 6, 7]), (0, [-5, 2, 9, -1, 3, 4])]

    rng = random.Random(20261020)

    def expression(depth):
        if not depth or rng.random() < 0.25:
            return rng.choice(['n', str(rng.randint(-10, 10))])
        op = rng.choice(['+', '-', '*', '/', '<', '<=', '>', '>=', '==', '!='])
        lhs = expression(depth - 1)
        rhs = str(rng.randint(1, 9)) if op == '/' else expression(depth - 1)
        return f'({lhs} {op} {rhs})'

    for index in range(random_cases):
        expr = expression(4)
        yield f'random_{index:03}', PREFIX + f'int main_function(int n) {{ return {expr}; }}', regular


INVALID = [
    ('undeclared', 'int f() { return missing; }', 5, 'declared'),
    ('duplicate', 'int f() { int x; int x; return 0; }', 5, 'double declaration'),
    ('duplicate_parameter', 'int f(int n) { int n; return 0; }', 5, 'double declaration'),
    ('out_of_scope', 'int f() { { int x; x = 1; } return x; }', 5, 'declared'),
    ('use_before_declaration', 'int f() { x = 1; int x; return x; }', 5, 'declared'),
    ('unknown_call', 'int f() { return other(); }', 5, 'unknown function'),
    ('void_assignment', 'int f() { int x; x = print(1); return x; }', 5, 'void function'),
    ('void_return', 'int f() { return print(1); }', 5, 'void function'),
    ('void_condition', 'int f() { if (print(1)) return 2; return 0; }', 5, 'void function'),
    ('nested_print', 'int f() { print(print(1)); return 0; }', 5, 'void function'),
    ('missing_semicolon', 'int f() { return 1 }', 4, 'syntax error'),
    ('bad_token', 'int f() { return @; }', 1, 'invalid syntax'),
    ('print_arity', 'int f() { print(); return 0; }', 4, 'syntax error'),
    ('read_arity', 'int f() { return read(1); }', 4, 'syntax error'),
    ('unterminated_comment', 'int f() { /* unfinished', 4, 'unterminated comment'),
    ('integer_range', 'int f() { return 99999999999999999999999; }', 4, 'out of range'),
    ('void_definition', 'void f() { print(1); }', 4, 'int function return type'),
    ('constant_zero_division', 'int f() { return 1 / 0; }', 5, 'division by zero'),
    ('computed_zero_divisor', 'int f(int n) { return n / (3 - 3); }', 5, 'division by zero'),
    ('constant_division_overflow', 'int f() { return (-2147483647 - 1) / -1; }', 5, 'division overflow'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--random-cases', type=int, default=100)
    parser.add_argument('--native', action='store_true',
                        help='also link and run every generated program natively on x86 Linux (requires multilib)')
    args = parser.parse_args()
    if args.native and sys.platform != 'linux':
        parser.error('--native requires x86 Linux; omit it to test via emulation on macOS.')
    try:
        import elftools  # noqa: F401
        import unicorn  # noqa: F401
    except ImportError:
        parser.error('install tests/requirements.txt in .venv, then run make test.')
    program_count = execution_count = rejection_count = 0
    native_count = 0
    failures = []
    with tempfile.TemporaryDirectory(prefix='minic-tests-') as directory:
        directory = Path(directory)
        for name, source, runs in cases(args.random_cases):
            try:
                path = directory / f'{name}.c'
                path.write_text(source)
                checked([str(ROOT / 'minic'), str(path)])
                obj, reference = directory / f'{name}.o', directory / f'{name}-reference'
                checked([clang(), '-target', 'i386-pc-linux-gnu', '-c',
                         str(path.with_suffix('.s')), '-o', str(obj)])
                # The reference executable uses the host architecture. Only the
                # builtin symbols are renamed to avoid interposing libc read().
                checked([clang(), '-O0', '-Dread=minic_read', '-Dprint=minic_print',
                         str(path), str(ROOT / 'runtime.c'), '-o', str(reference)])
                native = directory / f'{name}-native'
                if args.native:
                    checked([clang(), '-m32', '-no-pie', str(path.with_suffix('.s')),
                             str(ROOT / 'runtime.c'), '-o', str(native)])
                for argument, inputs in runs:
                    expected = checked([str(reference), str(argument)], input='\n'.join(map(str, inputs)) + '\n')
                    expected_output = list(map(int, expected.stdout.split()))
                    expected_return = int(re.search(r'return=(-?\d+)', expected.stderr)[1])
                    actual = execute_object(obj, argument, inputs)
                    if actual != (expected_output, expected_return):
                        raise AssertionError(f'argument={argument}, input={inputs}: '
                                             f'got {actual}, expected {(expected_output, expected_return)}')
                    execution_count += 1
                    if args.native:
                        result = checked([str(native), str(argument)], input='\n'.join(map(str, inputs)) + '\n')
                        native_output = list(map(int, result.stdout.split()))
                        native_return = int(re.search(r'return=(-?\d+)', result.stderr)[1])
                        if (native_output, native_return) != (expected_output, expected_return):
                            raise AssertionError(f'native argument={argument}, input={inputs}: '
                                                 f'got {(native_output, native_return)}, '
                                                 f'expected {(expected_output, expected_return)}')
                        native_count += 1
                program_count += 1
                print(f'PASS {name}', flush=True)
            except Exception as error:
                failures.append(f'{name}: {error}')
                print(f'FAIL {name}: {error}', flush=True)
        for name, body, status, message in INVALID:
            path = directory / f'invalid_{name}.c'
            path.write_text(PREFIX + body)
            result = subprocess.run([str(ROOT / 'minic'), str(path)], capture_output=True, text=True, timeout=10)
            if result.returncode != status or message not in result.stderr.lower() or path.with_suffix('.s').exists():
                failures.append(f'{name}: expected rejection {status}/{message!r}, '
                                f'got {result.returncode}: {result.stdout}{result.stderr}')
            else:
                rejection_count += 1
        for command, status in [([], 1), (['bad.txt'], 2), ([str(directory / 'missing.c')], 3)]:
            result = subprocess.run([str(ROOT / 'minic'), *command], capture_output=True, timeout=10)
            if result.returncode != status:
                failures.append(f'CLI {command}: expected exit {status}, got {result.returncode}')
            else:
                rejection_count += 1
    print(f'\n{program_count} programs, {execution_count} emulated / {native_count} native executions compared with Clang, '
          f'{rejection_count} expected rejections, {len(failures)} failures.')
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
