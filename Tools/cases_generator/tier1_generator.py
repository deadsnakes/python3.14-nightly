"""Generate the main interpreter switch.
Reads the instruction definitions from bytecodes.c.
Writes the cases to generated_cases.c.h, which is #included in ceval.c.
"""

import argparse

from analyzer import (
    Analysis,
    Instruction,
    Uop,
    Part,
    analyze_files,
    Skip,
    Flush,
    analysis_error,
    StackItem,
)
from generators_common import (
    DEFAULT_INPUT,
    ROOT,
    write_header,
    emit_tokens,
    type_and_null,
)
from cwriter import CWriter
from typing import TextIO
from stack import Stack, StackError


DEFAULT_OUTPUT = ROOT / "Python/generated_cases.c.h"


FOOTER = "#undef TIER_ONE\n"

def declare_variable(var: StackItem, out: CWriter) -> None:
    type, null = type_and_null(var)
    space = " " if type[-1].isalnum() else ""
    if var.condition:
        out.emit(f"{type}{space}{var.name} = {null};\n")
    else:
        out.emit(f"{type}{space}{var.name};\n")


def declare_variables(inst: Instruction, out: CWriter) -> None:
    stack = Stack()
    for part in inst.parts:
        if not isinstance(part, Uop):
            continue
        try:
            for var in reversed(part.stack.inputs):
                stack.pop(var)
            for var in part.stack.outputs:
                 stack.push(var)
        except StackError as ex:
            raise analysis_error(ex.args[0], part.body[0]) from None
    required = set(stack.defined)
    for part in inst.parts:
        if not isinstance(part, Uop):
            continue
        for var in part.stack.inputs:
            if var.name in required:
                required.remove(var.name)
                declare_variable(var, out)
        for var in part.stack.outputs:
            if var.name in required:
                required.remove(var.name)
                declare_variable(var, out)

def write_uop(
    uop: Part, out: CWriter, offset: int, stack: Stack, inst: Instruction, braces: bool
) -> int:
    # out.emit(stack.as_comment() + "\n")
    if isinstance(uop, Skip):
        entries = "entries" if uop.size > 1 else "entry"
        out.emit(f"/* Skip {uop.size} cache {entries} */\n")
        return offset + uop.size
    if isinstance(uop, Flush):
        out.emit(f"// flush\n")
        stack.flush(out)
        return offset
    try:
        out.start_line()
        if braces:
            out.emit(f"// {uop.name}\n")
        for var in reversed(uop.stack.inputs):
            out.emit(stack.pop(var))
        if braces:
            out.emit("{\n")
        if not uop.properties.stores_sp:
            for i, var in enumerate(uop.stack.outputs):
                out.emit(stack.push(var))
        for cache in uop.caches:
            if cache.name != "unused":
                if cache.size == 4:
                    type = "PyObject *"
                    reader = "read_obj"
                else:
                    type = f"uint{cache.size*16}_t "
                    reader = f"read_u{cache.size*16}"
                out.emit(
                    f"{type}{cache.name} = {reader}(&this_instr[{offset}].cache);\n"
                )
                if inst.family is None:
                    out.emit(f"(void){cache.name};\n")
            offset += cache.size
        emit_tokens(out, uop, stack, inst)
        if uop.properties.stores_sp:
            for i, var in enumerate(uop.stack.outputs):
                out.emit(stack.push(var))
        if braces:
            out.start_line()
            out.emit("}\n")
        # out.emit(stack.as_comment() + "\n")
        return offset
    except StackError as ex:
        raise analysis_error(ex.args[0], uop.body[0]) from None


def uses_this(inst: Instruction) -> bool:
    if inst.properties.needs_this:
        return True
    for uop in inst.parts:
        if not isinstance(uop, Uop):
            continue
        for cache in uop.caches:
            if cache.name != "unused":
                return True
    return False


def generate_tier1(
    filenames: list[str], analysis: Analysis, outfile: TextIO, lines: bool
) -> None:
    write_header(__file__, filenames, outfile)
    outfile.write(
        """
#ifdef TIER_TWO
    #error "This file is for Tier 1 only"
#endif
#define TIER_ONE 1
"""
    )
    out = CWriter(outfile, 2, lines)
    out.emit("\n")
    for name, inst in sorted(analysis.instructions.items()):
        needs_this = uses_this(inst)
        out.emit("\n")
        out.emit(f"TARGET({name}) {{\n")
        unused_guard = "(void)this_instr;\n" if inst.family is None else ""
        if inst.properties.needs_prev:
            out.emit(f"_Py_CODEUNIT *prev_instr = frame->instr_ptr;\n")
        if needs_this and not inst.is_target:
            out.emit(f"_Py_CODEUNIT *this_instr = frame->instr_ptr = next_instr;\n")
            out.emit(unused_guard)
        else:
            out.emit(f"frame->instr_ptr = next_instr;\n")
        out.emit(f"next_instr += {inst.size};\n")
        out.emit(f"INSTRUCTION_STATS({name});\n")
        if inst.is_target:
            out.emit(f"PREDICTED({name});\n")
            if needs_this:
                out.emit(f"_Py_CODEUNIT *this_instr = next_instr - {inst.size};\n")
                out.emit(unused_guard)
        if inst.family is not None:
            out.emit(
                f"static_assert({inst.family.size} == {inst.size-1}"
                ', "incorrect cache size");\n'
            )
        declare_variables(inst, out)
        offset = 1  # The instruction itself
        stack = Stack()
        for part in inst.parts:
            # Only emit braces if more than one uop
            insert_braces = len([p for p in inst.parts if isinstance(p, Uop)]) > 1
            offset = write_uop(part, out, offset, stack, inst, insert_braces)
        out.start_line()
        if not inst.parts[-1].properties.always_exits:
            stack.flush(out)
            if inst.parts[-1].properties.ends_with_eval_breaker:
                out.emit("CHECK_EVAL_BREAKER();\n")
            out.emit("DISPATCH();\n")
        out.start_line()
        out.emit("}")
        out.emit("\n")
    outfile.write(FOOTER)


arg_parser = argparse.ArgumentParser(
    description="Generate the code for the interpreter switch.",
    formatter_class=argparse.ArgumentDefaultsHelpFormatter,
)

arg_parser.add_argument(
    "-o", "--output", type=str, help="Generated code", default=DEFAULT_OUTPUT
)

arg_parser.add_argument(
    "-l", "--emit-line-directives", help="Emit #line directives", action="store_true"
)

arg_parser.add_argument(
    "input", nargs=argparse.REMAINDER, help="Instruction definition file(s)"
)


def generate_tier1_from_files(
    filenames: list[str], outfilename: str, lines: bool
) -> None:
    data = analyze_files(filenames)
    with open(outfilename, "w") as outfile:
        generate_tier1(filenames, data, outfile, lines)


if __name__ == "__main__":
    args = arg_parser.parse_args()
    if len(args.input) == 0:
        args.input.append(DEFAULT_INPUT)
    data = analyze_files(args.input)
    with open(args.output, "w") as outfile:
        generate_tier1(args.input, data, outfile, args.emit_line_directives)
