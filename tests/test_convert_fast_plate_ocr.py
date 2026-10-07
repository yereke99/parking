"""Tests for tools/convert_fast_plate_ocr.py on small hand-encoded ONNX protobufs.

The converter deliberately avoids the `onnx` package, so these tests build their models with a
few lines of protobuf encoding instead of depending on it.
"""

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "convert_fast_plate_ocr", str(ROOT / "tools" / "convert_fast_plate_ocr.py"))
converter = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = converter
SPEC.loader.exec_module(converter)

FLOAT, UINT8, INT32 = 1, 2, 6


def varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def field(number, payload):
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    return varint(number << 3 | 2) + varint(len(payload)) + payload


def int_field(number, value):
    return varint(number << 3) + varint(value)


def value_info(name, elem_type):
    shape = field(2, field(1, int_field(1, 1)))
    return field(1, name) + field(2, field(1, int_field(1, elem_type) + shape))


def node(op_type, inputs, outputs, cast_to=None):
    body = b"".join(field(1, name) for name in inputs)
    body += b"".join(field(2, name) for name in outputs)
    body += field(4, op_type)
    if cast_to is not None:
        body += field(5, field(1, "to") + int_field(3, cast_to) + int_field(20, 2))
    return body


def model(nodes, inputs, outputs, initializers=()):
    graph = b"".join(field(1, item) for item in nodes) + field(2, "graph")
    graph += b"".join(field(5, field(8, name)) for name in initializers)
    graph += b"".join(field(11, item) for item in inputs)
    graph += b"".join(field(12, item) for item in outputs)
    opset = field(8, int_field(2, 15))
    return int_field(1, 8) + field(7, graph) + opset


def fast_plate_like(input_type=UINT8, cast_to=FLOAT, consumer="Cast"):
    nodes = [node(consumer, ["input"], ["x"], cast_to if consumer == "Cast" else None),
             node("Relu", ["x"], ["plate"])]
    return model(nodes, [value_info("input", input_type)], [value_info("plate", FLOAT)])


class ConvertFastPlateOcrTests(unittest.TestCase):
    def test_retypes_only_the_input_byte(self):
        original = fast_plate_like()
        data = bytearray(original)
        summary = converter.convert(data)
        self.assertIn("uint8 -> float32", summary)
        self.assertEqual(len(data), len(original))
        self.assertEqual(sum(a != b for a, b in zip(original, data)), 1)
        name, locations, consumers = converter.describe(data)
        self.assertEqual(name, "input")
        self.assertEqual([value for _, _, value in locations], [FLOAT])
        self.assertEqual(consumers, [("Cast", "", FLOAT)])

    def test_matching_value_info_is_retyped_too(self):
        nodes = [node("Cast", ["input"], ["x"], FLOAT), node("Relu", ["x"], ["plate"])]
        graph = b"".join(field(1, item) for item in nodes) + field(2, "graph")
        graph += field(11, value_info("input", UINT8)) + field(12, value_info("plate", FLOAT))
        graph += field(13, value_info("input", UINT8))
        data = bytearray(int_field(1, 8) + field(7, graph))
        converter.convert(data)
        _, locations, _ = converter.describe(data)
        self.assertEqual([value for _, _, value in locations], [FLOAT, FLOAT])

    def test_already_float_model_is_left_alone(self):
        original = fast_plate_like(input_type=FLOAT)
        data = bytearray(original)
        self.assertIn("already float32", converter.convert(data))
        self.assertEqual(bytes(data), original)

    def test_refuses_input_read_by_anything_but_cast_to_float(self):
        for kwargs in ({"consumer": "Relu"}, {"cast_to": INT32}):
            with self.subTest(**kwargs):
                data = bytearray(fast_plate_like(**kwargs))
                with self.assertRaises(converter.ModelError):
                    converter.convert(data)

    def test_initializers_listed_as_inputs_are_not_image_inputs(self):
        nodes = [node("Cast", ["input"], ["x"], FLOAT), node("Add", ["x", "bias"], ["plate"])]
        data = bytearray(model(nodes, [value_info("input", UINT8), value_info("bias", FLOAT)],
                               [value_info("plate", FLOAT)], initializers=["bias"]))
        converter.convert(data)
        self.assertEqual(converter.describe(data)[1][0][2], FLOAT)

    def test_refuses_two_image_inputs(self):
        nodes = [node("Cast", ["a"], ["x"], FLOAT), node("Cast", ["b"], ["y"], FLOAT)]
        data = bytearray(model(nodes, [value_info("a", UINT8), value_info("b", UINT8)],
                               [value_info("x", FLOAT)]))
        with self.assertRaises(converter.ModelError):
            converter.convert(data)

    def test_truncated_or_foreign_file_is_reported_not_crashed(self):
        original = fast_plate_like()
        for data in (original[:-3], b"\xff" * 16, b"not an onnx model at all"):
            with self.subTest(data=data[:8]):
                with self.assertRaises(converter.ModelError):
                    converter.convert(bytearray(data))

    def test_command_line_writes_converted_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            source = os.path.join(directory, "model.onnx")
            output = os.path.join(directory, "model_float.onnx")
            with open(source, "wb") as handle:
                handle.write(fast_plate_like())
            self.assertEqual(converter.main([source, output]), 0)
            with open(output, "rb") as handle:
                self.assertEqual(converter.describe(bytearray(handle.read()))[1][0][2], FLOAT)
            with open(source, "wb") as handle:
                handle.write(b"garbage")
            self.assertEqual(converter.main([source, output + ".bad"]), 1)
            self.assertFalse(os.path.exists(output + ".bad"))


if __name__ == "__main__":
    unittest.main()
