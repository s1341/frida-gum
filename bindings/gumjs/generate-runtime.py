#!/usr/bin/env python

from __future__ import unicode_literals, print_function
import codecs
import os
import platform
import subprocess
import sys

def generate_runtime_v8(output_dir, output, inputs):
    with codecs.open(os.path.join(output_dir, output), 'wb', 'utf-8') as output_file:
        output_file.write("""\
#include "gumv8bundle.h"

static const {entry_type} {entries_identifier}[] =
{{""".format(entry_type="GumV8Source",
            entries_identifier=underscorify(output) + "_sources"))

        for input_path in inputs:
            input_name = os.path.basename(input_path)
            output_file.write("""
  {{
    "{filename}",
    {{
""".format(filename=input_name))
            with codecs.open(input_path, 'rb', 'utf-8') as input_file:
                write_code(input_file.read(), output_file)
            output_file.write("      NULL\n    }\n  },\n")

        output_file.write("\n  { NULL, { NULL } }\n};")

def generate_runtime_duk(output_dir, output, input_dir, inputs):
    with codecs.open(os.path.join(output_dir, output), 'wb', 'utf-8') as output_file:
        output_file.write("""\
#include "gumdukbundle.h"
""")

        build_os = platform.system().lower()

        if build_os == 'windows':
            program_suffix = ".exe"
        else:
            program_suffix = ""

        dukcompile = os.path.join(output_dir, "gumdukcompile" + program_suffix)
        dukcompile_sources = list(map(lambda name: os.path.join(input_dir, name), ["gumdukcompile.c", "duktape.c"]))
        if build_os == 'windows':
            subprocess.call(["cl.exe",
                "/nologo", "/MT", "/W3", "/O1", "/GL", "/MP",
                "/D", "WIN32",
                "/D", "_WINDOWS",
                "/D", "WINVER=0x0501",
                "/D", "_WIN32_WINNT=0x0501",
                "/D", "NDEBUG",
                "/D", "_CRT_SECURE_NO_WARNINGS",
                "/D", "_USING_V110_SDK71_"] + dukcompile_sources, cwd=output_dir)
        else:
            dukcompile_libs = []
            if build_os == 'darwin':
                sdk = "macosx"
                CC = [
                    subprocess.check_output(["xcrun", "--sdk", sdk, "-f", "clang"]).decode('utf-8').rstrip("\n"),
                    "-isysroot", subprocess.check_output(["xcrun", "--sdk", sdk, "--show-sdk-path"]).decode('utf-8').rstrip("\n")
                ]
            else:
                CC = ["gcc"]
                dukcompile_libs.append("-lm")
            subprocess.call(CC + ["-Wall", "-pipe", "-O2", "-fomit-frame-pointer"] +
                dukcompile_sources +
                ["-o", dukcompile] + dukcompile_libs)

        modules = []
        for input_path in inputs:
            input_name = os.path.basename(input_path)

            base, ext = os.path.splitext(input_name)

            input_name_duk = base + ".duk"
            input_path_duk = os.path.join(output_dir, input_name_duk)

            input_identifier = "gum_duk_script_runtime_module_" + identifier(base)

            subprocess.call([dukcompile, input_path, input_path_duk])

            with open(input_path_duk, 'rb') as duk:
                code = duk.read()
                size = len(code)
                output_file.write("\nstatic const guint8 " + input_identifier + "[" + str(size) + "] =\n{")
                write_bytes(code, output_file)
                output_file.write("\n};\n")
                modules.append((input_identifier, size))

        output_file.write("\nstatic const {entry_type} {entries_identifier}[] =\n{{\n  ".format(
            entry_type="GumDukRuntimeModule",
            entries_identifier=underscorify(output) + "_modules"))
        output_file.write("\n  ".join(map(lambda e: "{{ {identifier}, {size} }},".format(identifier=e[0], size=e[1]), modules)))
        output_file.write("\n  { NULL, 0 }\n};")

def write_code(js_code, sink):
    MAX_LINE_LENGTH = 80
    INDENT = 6
    QUOTATION_OVERHEAD = 2
    LINE_OVERHEAD = 1
    NULL_TERMINATOR_SIZE = 1
    MAX_CHARACTER_SIZE = 4
    # MSVC's limit is roughly 65535 bytes, but we'll play it safe
    MAX_LITERAL_SIZE = 32768

    # MSVC's individual quoted string limit is 2048 bytes
    assert MAX_LINE_LENGTH <= 2048 / MAX_CHARACTER_SIZE

    pending = js_code.replace('\\', '\\\\').replace('"', '\\"').replace("\n", "\\n").replace("??", "\\?\\?")
    size = 0
    while len(pending) > 0:
        chunk_length = min(MAX_LINE_LENGTH - INDENT - QUOTATION_OVERHEAD, len(pending))
        while True:
            chunk = pending[:chunk_length]
            chunk_size = len(chunk.encode('utf-8'))
            if chunk[-1] != "\\" and size + chunk_size + LINE_OVERHEAD + NULL_TERMINATOR_SIZE <= MAX_LITERAL_SIZE:
                pending = pending[chunk_length:]
                size += chunk_size + LINE_OVERHEAD
                break
            chunk_length -= 1
        sink.write((" " * INDENT) + "\"" + chunk + "\"")
        capacity = MAX_LITERAL_SIZE - size
        if capacity < INDENT + QUOTATION_OVERHEAD + (2 * MAX_CHARACTER_SIZE) + LINE_OVERHEAD + NULL_TERMINATOR_SIZE:
            sink.write(",")
            size = 0
        if len(pending) > 0:
            sink.write("\n")
    if size != 0:
        sink.write(",")
    sink.write("\n")

def write_bytes(data, sink):
    sink.write("\n  ")
    line_length = 0
    offset = 0
    for b in bytearray(data):
        if offset > 0:
            sink.write(",")
            line_length += 1
        if line_length >= 70:
            sink.write("\n  ")
            line_length = 0
        token = str(b)
        sink.write(token)

        line_length += len(token)
        offset += 1

def underscorify(filename):
    if filename.startswith("gumv8"):
        result = "gum_v8_"
        filename = filename[5:]
    elif filename.startswith("gumduk"):
        result = "gum_duk_"
        filename = filename[6:]
    else:
        result = ""
    return result + os.path.splitext(filename)[0].lower().replace("-", "_")

def identifier(filename):
    result = ""
    if filename.startswith("gumjs-"):
        filename = filename[6:]
    for c in filename:
        if c.isalnum():
            result += c.lower()
        else:
            result += "_"
    return result

def node_script_path(name):
    return os.path.join(".", "node_modules", ".bin", name + script_suffix())

def script_suffix():
    build_os = platform.system().lower()
    return ".cmd" if build_os == 'windows' else ""


if __name__ == '__main__':
    input_dir = sys.argv[1]
    output_dir = sys.argv[2]

    runtime = os.path.abspath(os.path.join(output_dir, "gumjs-runtime.js"))

    subprocess.call([node_script_path("frida-compile"), "-c", "./runtime", "-o", runtime], cwd=input_dir)

    polyfill_modules = [os.path.join(input_dir, input_name) for input_name in [
        "gumjs-regenerator.js",
    ]]

    generate_runtime_v8(output_dir, "gumv8script-runtime.h", polyfill_modules + [runtime])
    generate_runtime_v8(output_dir, "gumv8script-debug.h", [os.path.join(input_dir, "gumjs-debug.js")])

    duk_polyfill_modules = [os.path.join(input_dir, input_name) for input_name in [
        "gumjs-babel-polyfill.js",
    ]] + polyfill_modules
    generate_runtime_duk(output_dir, "gumdukscript-runtime.h", input_dir, duk_polyfill_modules + [runtime])
