#!/usr/bin/env python3
"""Compile MiniC and execute its actual i386 assembly on any host.

Only the read/print runtime is simulated. The generated instructions run in
Unicorn, including calls, stack frames, arithmetic, branches and spills.
"""
import argparse
import io
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def clang():
    executable = os.environ.get("CLANG") or shutil.which("clang")
    if not executable:
        raise RuntimeError("Clang is required to assemble the i386 output (set CLANG=/path/to/clang).")
    return executable


def checked(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, timeout=30, **kwargs)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {' '.join(map(str, command))}\n"
                           f"{result.stdout}{result.stderr}")
    return result


def execute_object(path, argument=0, inputs=()):
    try:
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
        from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX,
                                      UC_X86_REG_ECX, UC_X86_REG_EDX,
                                      UC_X86_REG_ESI, UC_X86_REG_EDI,
                                      UC_X86_REG_EBP, UC_X86_REG_ESP,
                                      UC_X86_REG_EIP, UC_X86_REG_EFLAGS)
    except ImportError as error:
        raise RuntimeError("Install the test dependencies first:\n"
                           "  python3 -m venv .venv\n"
                           "  .venv/bin/python -m pip install -r tests/requirements.txt\n"
                           "Then run this script with .venv/bin/python.") from error

    code_base, stack_base, stack_size = 0x100000, 0x200000, 0x100000
    read_address, print_address, stop_address = 0x400000, 0x400010, 0x400020
    elf = ELFFile(io.BytesIO(Path(path).read_bytes()))
    if elf['e_machine'] != 'EM_386' or elf['e_type'] != 'ET_REL':
        raise RuntimeError("Expected an i386 ELF relocatable object.")
    text = bytearray(elf.get_section_by_name('.text').data())
    symbols = elf.get_section_by_name('.symtab')
    text_index = elf.get_section_index('.text')
    externs = {'read': read_address, 'print': print_address}
    relocations = elf.get_section_by_name('.rel.text')
    if relocations:
        for relocation in relocations.iter_relocations():
            symbol = symbols.get_symbol(relocation['r_info_sym'])
            if symbol['st_shndx'] == text_index:
                address = code_base + symbol['st_value']
            elif symbol.name in externs:
                address = externs[symbol.name]
            else:
                raise RuntimeError(f"Unresolved assembly symbol: {symbol.name}")
            offset = relocation['r_offset']
            addend = struct.unpack_from('<I', text, offset)[0]
            kind = relocation['r_info_type']
            if kind in (2, 4):  # R_386_PC32, R_386_PLT32
                value = address + addend - (code_base + offset)
            elif kind == 1:  # R_386_32
                value = address + addend
            else:
                raise RuntimeError(f"Unsupported ELF relocation: {kind}")
            struct.pack_into('<I', text, offset, value & 0xffffffff)

    machine = Uc(UC_ARCH_X86, UC_MODE_32)
    machine.mem_map(code_base, ((len(text) + 4095) // 4096) * 4096)
    machine.mem_write(code_base, bytes(text))
    machine.mem_map(stack_base, stack_size)
    machine.mem_map(read_address, 4096)
    machine.mem_write(read_address, b'\xc3')
    machine.mem_write(print_address, b'\xc3')
    sp = stack_base + stack_size - 20  # i386 function-entry ESP is 12 mod 16.
    machine.mem_write(sp, struct.pack('<II', stop_address, argument & 0xffffffff))
    machine.reg_write(UC_X86_REG_ESP, sp)
    preserved = {UC_X86_REG_EBX: 0x12345678, UC_X86_REG_ESI: 0x23456789,
                 UC_X86_REG_EDI: 0x34567890, UC_X86_REG_EBP: 0x456789ab}
    for register, value in preserved.items():
        machine.reg_write(register, value)
    output, pending = [], iter(inputs)

    def signed(value):
        return value - 0x100000000 if value & 0x80000000 else value

    def hook(uc, address, size, data):
        if address not in (read_address, print_address):
            return
        runtime_sp = uc.reg_read(UC_X86_REG_ESP)
        if runtime_sp % 16 != 12:
            raise RuntimeError("Generated call violates the i386 16-byte stack alignment.")
        if address == print_address:
            output.append(signed(struct.unpack('<I', uc.mem_read(runtime_sp + 4, 4))[0]))
            result = 0
        else:
            try:
                result = next(pending)
            except StopIteration as error:
                raise RuntimeError("Program requested more input than was supplied.") from error
        # A real C runtime may clobber all caller-saved registers and flags.
        uc.reg_write(UC_X86_REG_ECX, 0x76543210)
        uc.reg_write(UC_X86_REG_EDX, 0x65432109)
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
        uc.reg_write(UC_X86_REG_EAX, result & 0xffffffff)

    machine.hook_add(UC_HOOK_CODE, hook)
    entry = symbols.get_symbol_by_name('main_function')
    if not entry:
        raise RuntimeError("Assembly does not export the MiniC main_function entry point.")
    machine.emu_start(code_base + entry[0]['st_value'], stop_address,
                      timeout=5_000_000, count=2_000_000)
    if machine.reg_read(UC_X86_REG_EIP) != stop_address:
        raise RuntimeError("Program did not return within the execution limit.")
    if machine.reg_read(UC_X86_REG_ESP) != sp + 4:
        raise RuntimeError("Generated code did not restore the stack pointer.")
    for register, value in preserved.items():
        if machine.reg_read(register) != value:
            raise RuntimeError("Generated code clobbered a callee-saved register.")
    return output, signed(machine.reg_read(UC_X86_REG_EAX))


def compile_and_run(source, argument=0, inputs=()):
    checked([str(ROOT / 'minic'), str(source)])
    with tempfile.TemporaryDirectory(prefix='minic-object-') as directory:
        obj = Path(directory) / 'program.o'
        checked([clang(), '-target', 'i386-pc-linux-gnu', '-c',
                 str(Path(source).with_suffix('.s')), '-o', str(obj)])
        return execute_object(obj, argument, inputs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('argument', type=int, nargs='?', default=0)
    parser.add_argument('--input', type=int, nargs='*', default=[])
    args = parser.parse_args()
    try:
        output, result = compile_and_run(args.source.resolve(), args.argument, args.input)
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'{error}\n')
    for value in output:
        print(value)
    print(f'return={result}', file=sys.stderr)


if __name__ == '__main__':
    main()
