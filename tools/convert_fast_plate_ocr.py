#!/usr/bin/env python3
"""Retype the Fast Plate OCR model input from uint8 to float32 so TensorRT 8.2 can load it.

The released Fast Plate OCR ONNX models take raw uint8 pixels, and the first node of the graph
casts them to float32 before the model's own 1/255 rescaling. TensorRT 8.2, the version JetPack 4
ships for the Jetson Nano, has no uint8 tensor type, so its ONNX parser rejects the model and the
recognizer had to run on ONNX Runtime on the CPU.

This tool changes only the declared element type of that one graph input. The C++ runtime then
feeds the same 0..255 pixel values as float32 and the leading Cast becomes a float32 -> float32
no-op, so the network computes exactly what it computed before. The change is a one-byte patch of
the serialised protobuf: every weight and node stays byte-for-byte identical and no `onnx` package
is needed (the Jetson image runs this at build time with plain Python 3.6).

The patch is refused unless every node that reads the input is a Cast to float32, because for any
other consumer a different input type would change what the model computes.

Usage: convert_fast_plate_ocr.py cct_s_v2_global.onnx cct_s_v2_global_float.onnx
"""

import argparse
import os
import sys
import tempfile

# onnx.TensorProto.DataType values.
FLOAT = 1
UINT8 = 2

# Field numbers from onnx.proto; unchanged since ONNX IR version 3.
MODEL_GRAPH = 7
GRAPH_NODE = 1
GRAPH_INITIALIZER = 5
GRAPH_INPUT = 11
GRAPH_OUTPUT = 12
GRAPH_VALUE_INFO = 13
NODE_INPUT = 1
NODE_OP_TYPE = 4
NODE_ATTRIBUTE = 5
NODE_DOMAIN = 7
ATTRIBUTE_NAME = 1
ATTRIBUTE_INT = 3
ATTRIBUTE_GRAPH = 6
ATTRIBUTE_GRAPHS = 11
TENSOR_NAME = 8
VALUE_INFO_NAME = 1
VALUE_INFO_TYPE = 2
TYPE_TENSOR = 1
TENSOR_TYPE_ELEM_TYPE = 1

# Protobuf wire types.
VARINT = 0
FIXED64 = 1
LENGTH_DELIMITED = 2
FIXED32 = 5


class ModelError(Exception):
    """The file is not an ONNX model this tool can safely patch."""


def read_varint(data, pos, end):
    value = 0
    shift = 0
    while True:
        if pos >= end or shift > 63:
            raise ModelError("malformed protobuf varint at byte %d" % pos)
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, pos
        shift += 7


def fields(data, start, end):
    """Every field of one message as (number, wire_type, value_start, value_end, varint)."""
    result = []
    pos = start
    while pos < end:
        key, pos = read_varint(data, pos, end)
        number, wire = key >> 3, key & 7
        if wire == VARINT:
            value, after = read_varint(data, pos, end)
            result.append((number, wire, pos, after, value))
            pos = after
            continue
        if wire == LENGTH_DELIMITED:
            length, pos = read_varint(data, pos, end)
            after = pos + length
        elif wire == FIXED64:
            after = pos + 8
        elif wire == FIXED32:
            after = pos + 4
        else:
            raise ModelError("unsupported protobuf wire type %d at byte %d" % (wire, pos))
        if after > end:
            raise ModelError("protobuf field %d overruns its message at byte %d" % (number, pos))
        result.append((number, wire, pos, after, None))
        pos = after
    return result


def decode(raw):
    try:
        return bytes(raw).decode("utf-8")
    except UnicodeDecodeError:
        raise ModelError("a protobuf string field is not UTF-8")


def messages(data, start, end, number):
    return [(s, e) for n, w, s, e, _ in fields(data, start, end)
            if n == number and w == LENGTH_DELIMITED]


def text(data, start, end, number):
    """A string field; protobuf keeps the last value when a field repeats."""
    values = messages(data, start, end, number)
    if not values:
        return ""
    s, e = values[-1]
    return decode(data[s:e])


def elem_types(data, start, end):
    """(offset, length, value) of every TypeProto.tensor_type.elem_type in a ValueInfoProto."""
    found = []
    for type_start, type_end in messages(data, start, end, VALUE_INFO_TYPE):
        for tensor_start, tensor_end in messages(data, type_start, type_end, TYPE_TENSOR):
            for n, w, s, e, value in fields(data, tensor_start, tensor_end):
                if n == TENSOR_TYPE_ELEM_TYPE and w == VARINT:
                    found.append((s, e - s, value))
    return found


def describe(data):
    """Locate the image input, its element type bytes and the nodes reading it."""
    graphs = messages(data, 0, len(data), MODEL_GRAPH)
    if len(graphs) != 1:
        raise ModelError("expected one ONNX graph, found %d" % len(graphs))
    graph_start, graph_end = graphs[0]
    graph = fields(data, graph_start, graph_end)

    def entries(number):
        return [(s, e) for n, w, s, e, _ in graph if n == number and w == LENGTH_DELIMITED]

    initializers = set(text(data, s, e, TENSOR_NAME) for s, e in entries(GRAPH_INITIALIZER))
    inputs = [(text(data, s, e, VALUE_INFO_NAME), s, e) for s, e in entries(GRAPH_INPUT)]
    inputs = [entry for entry in inputs if entry[0] not in initializers]
    if len(inputs) != 1:
        raise ModelError("expected one image input, found %d" % len(inputs))
    name, input_start, input_end = inputs[0]
    if name in set(text(data, s, e, VALUE_INFO_NAME) for s, e in entries(GRAPH_OUTPUT)):
        raise ModelError("input '%s' is also a graph output" % name)

    locations = elem_types(data, input_start, input_end)
    if len(locations) != 1:
        raise ModelError("input '%s' has no single tensor element type" % name)
    # value_info may repeat the input's type; it must be patched together with the input.
    for s, e in entries(GRAPH_VALUE_INFO):
        if text(data, s, e, VALUE_INFO_NAME) == name:
            locations.extend(elem_types(data, s, e))

    consumers = []
    for node_start, node_end in entries(GRAPH_NODE):
        node = fields(data, node_start, node_end)
        attributes = [(s, e) for n, w, s, e, _ in node if n == NODE_ATTRIBUTE and w == LENGTH_DELIMITED]
        for s, e in attributes:
            if any(n in (ATTRIBUTE_GRAPH, ATTRIBUTE_GRAPHS) for n, _, _, _, _ in fields(data, s, e)):
                # A subgraph may read the input by its outer-scope name; not checked here.
                raise ModelError("models with control-flow subgraphs are not supported")
        node_inputs = [decode(data[s:e]) for n, w, s, e, _ in node
                       if n == NODE_INPUT and w == LENGTH_DELIMITED]
        if name not in node_inputs:
            continue
        cast_to = None
        for s, e in attributes:
            if text(data, s, e, ATTRIBUTE_NAME) == "to":
                values = [v for n, w, _, _, v in fields(data, s, e) if n == ATTRIBUTE_INT and w == VARINT]
                cast_to = values[-1] if values else None
        consumers.append((text(data, node_start, node_end, NODE_OP_TYPE),
                          text(data, node_start, node_end, NODE_DOMAIN), cast_to))
    return name, locations, consumers


def convert(data):
    """Patch `data` (a bytearray) in place. Returns a one-line summary."""
    name, locations, consumers = describe(data)
    elem_type = locations[0][2]
    if elem_type == FLOAT:
        return "input '%s' is already float32; copied unchanged" % name
    if elem_type != UINT8:
        raise ModelError("input '%s' has element type %d, expected uint8 (2)" % (name, elem_type))
    if not consumers:
        raise ModelError("no node reads input '%s'" % name)
    for op_type, domain, cast_to in consumers:
        if op_type != "Cast" or domain not in ("", "ai.onnx") or cast_to != FLOAT:
            raise ModelError("input '%s' feeds %s(to=%s); only Cast to float32 keeps the model "
                             "unchanged" % (name, op_type, cast_to))
    for offset, length, value in locations:
        if value != UINT8 or length != 1:
            raise ModelError("input '%s' is declared with conflicting element types" % name)
        data[offset] = FLOAT
    return "retyped input '%s' uint8 -> float32 (%d Cast consumer%s)" % (
        name, len(consumers), "" if len(consumers) == 1 else "s")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("source", help="Fast Plate OCR ONNX model with a uint8 input")
    parser.add_argument("output", help="where to write the float32-input model")
    args = parser.parse_args(argv)

    with open(args.source, "rb") as handle:
        original = handle.read()
    data = bytearray(original)
    try:
        summary = convert(data)
        name, locations, _ = describe(data)
        if locations[0][2] != FLOAT or len(data) != len(original):
            raise ModelError("patched model failed its own re-check")
    except ModelError as error:
        print("convert_fast_plate_ocr: %s: %s" % (args.source, error), file=sys.stderr)
        return 1

    directory = os.path.dirname(os.path.abspath(args.output))
    handle, temporary = tempfile.mkstemp(prefix=".fast-plate-ocr-", dir=directory)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(bytes(data))
        os.chmod(temporary, 0o644)
        os.replace(temporary, args.output)
    except BaseException:
        if os.path.exists(temporary):
            os.unlink(temporary)
        raise
    changed = sum(1 for a, b in zip(original, data) if a != b)
    print("convert_fast_plate_ocr: %s; %d byte(s) changed; wrote %s" % (summary, changed, args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
