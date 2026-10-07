#!/usr/bin/env python3
"""Export the EasyOCR english_g2 recognizer to a fixed-shape ONNX model.

The C++ `easyocr_onnx` backend runs this model in-process with the detector's inference backend
(TensorRT on the Jetson), so no PyTorch worker is needed at run time. Preprocessing, the
allowlist and CTC decoding live in src/ocr/easyocr_onnx.cpp and reproduce EasyOCR 1.6.2's
`Reader.recognize` for one crop.

The input is the normalized grayscale crop, 1x1x64xWIDTH, padded on the right by repeating its
last column exactly as EasyOCR does. The output is raw logits, 1xTx97; class 0 is the CTC blank
and class i is CHARACTERS[i - 1].

Runs under the JetPack Python 3.6 / PyTorch 1.10 image as well as newer PyTorch.
"""

import argparse
import inspect
import sys
from collections import OrderedDict
from pathlib import Path

import torch
from torch import nn

# Must match kEasyOcrEnglishG2Characters in src/ocr/easyocr_onnx.cpp.
CHARACTERS = (
    "0123456789!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ €"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
)
MODEL_HEIGHT = 64


class Recognizer(nn.Module):
    """EasyOCR's generation-2 model for one crop, without its unused `text` argument.

    AdaptiveAvgPool2d((None, 1)) over the permuted feature map is a mean over the feature height;
    writing it as one keeps the graph free of adaptive pooling, which TensorRT 8.2 cannot parse.
    The LSTMs get explicit zero initial states (PyTorch's default), so they export as constants
    instead of a Shape/Gather/Expand chain built from the batch size.
    """

    def __init__(self, model: nn.Module) -> None:
        super().__init__()
        self.model = model
        for index, block in enumerate(model.SequenceModeling):
            self.register_buffer("state{0}".format(index),
                                 torch.zeros(2, 1, block.rnn.hidden_size))

    def forward(self, image: torch.Tensor) -> torch.Tensor:
        feature = self.model.FeatureExtraction(image)
        feature = feature.permute(0, 3, 1, 2).mean(dim=3)
        for index, block in enumerate(self.model.SequenceModeling):
            state = getattr(self, "state{0}".format(index))
            recurrent, _ = block.rnn(feature, (state, state))
            feature = block.linear(recurrent)
        return self.model.Prediction(feature.contiguous())


def load_model(weights: Path) -> nn.Module:
    from easyocr.model.vgg_model import Model

    try:
        from easyocr.config import recognition_models
        characters = recognition_models["gen2"]["english_g2"]["characters"]
    except (ImportError, KeyError):
        characters = CHARACTERS
    if characters != CHARACTERS:
        raise SystemExit("english_g2 character set differs from the C++ decoder's copy")

    model = Model(input_channel=1, output_channel=256, hidden_size=256,
                  num_class=len(CHARACTERS) + 1)
    state = torch.load(str(weights), map_location="cpu")
    # The checkpoint was saved from nn.DataParallel; EasyOCR strips the prefix the same way.
    model.load_state_dict(OrderedDict(
        (key[len("module."):] if key.startswith("module.") else key, value)
        for key, value in state.items()
    ))
    return model.eval()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weights", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="receives english_g2_<width>.onnx for every width")
    parser.add_argument("--widths", type=int, nargs="+", default=[64, 128, 192, 256, 320, 384],
                        help="input widths, multiples of 64; the C++ backend loads all six defaults")
    parser.add_argument("--opset", type=int, default=11)
    args = parser.parse_args()
    if any(width < MODEL_HEIGHT or width % MODEL_HEIGHT for width in args.widths):
        parser.error("--widths must be positive multiples of 64")

    model = load_model(args.weights)
    wrapper = Recognizer(model).eval()
    options = {}
    if "dynamo" in inspect.signature(torch.onnx.export).parameters:
        options["dynamo"] = False
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for width in args.widths:
        sample = torch.rand(1, 1, MODEL_HEIGHT, width) * 2.0 - 1.0
        with torch.no_grad():
            expected = model(sample, None)
            actual = wrapper(sample)
        difference = float((expected - actual).abs().max())
        if difference > 1e-4:
            raise SystemExit("export wrapper diverges from EasyOCR at width {0}: max |diff| = "
                             "{1}".format(width, difference))
        output = args.output_dir / "english_g2_{0}.onnx".format(width)
        with torch.no_grad():
            torch.onnx.export(
                wrapper, sample, str(output),
                input_names=["image"], output_names=["logits"],
                opset_version=args.opset, do_constant_folding=True, **options
            )
        print("exported {0}: input 1x1x{1}x{2}, output {3}".format(
            output, MODEL_HEIGHT, width, "x".join(str(d) for d in actual.shape)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
