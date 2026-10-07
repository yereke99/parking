#!/usr/bin/env python3
"""Export Nomeroff Net's Kazakhstan plate OCR model to ONNX for the C++ runtime.

Nomeroff 4.0.1 needs Python >= 3.9 and PyTorch >= 1.12, which JetPack 4 cannot provide, so the
`nomeroff` worker cannot run on the Jetson Nano. Its "kz" text reader is a small network, though:
a ResNet-18 trunk up to layer3, a linear layer, two bidirectional LSTMs and a linear CTC head
(NPOcrNet in nomeroff_net/nnmodels/ocr_model.py). This script rebuilds that network with the
image's own PyTorch 1.10, loads Nomeroff's published checkpoint and writes a batch-1 ONNX model,
which the `nomeroff_onnx` backend runs in-process on TensorRT.

The settings are Nomeroff's model card for "kz"
(https://models.vsp.net.ua/config_model/nomeroff-net-ocr-kz/model-4.json): resnet18 backbone,
3x50x200 input, linear 512, LSTM hidden 32, letters 0-9A-Z plus the CTC blank at index 0.

Before anything is written, the export graph is compared with a line-for-line copy of Nomeroff's
own forward pass on random inputs.

Usage: export_nomeroff_onnx.py --checkpoint anpr_ocr_kz_2022_11_14.ckpt --output kz.onnx
"""

import argparse
import os
import sys
import tempfile

import torch
from torch import nn
from torchvision.models import resnet18

HEIGHT = 50
WIDTH = 200
CHANNELS = 3
LINEAR_SIZE = 512
HIDDEN_SIZE = 32
LETTERS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
CLASSES = len(LETTERS) + 1  # the CTC blank is class 0


class BlockRNN(nn.Module):
    """Nomeroff's BlockRNN with its default bidirectional LSTM."""

    def __init__(self, in_size, hidden_size):
        super().__init__()
        self.rnn = nn.LSTM(in_size, hidden_size, bidirectional=True, batch_first=True)

    def forward(self, batch, add_output=False, state=None):
        outputs, _ = self.rnn(batch, state)
        if add_output:
            half = outputs.size(2) // 2
            outputs = outputs[:, :, :half] + outputs[:, :, half:]
        return outputs


class NomeroffKzOcr(nn.Module):
    """NPOcrNet's inference path, with the checkpoint's layer names."""

    def __init__(self):
        super().__init__()
        self.conv_nn = nn.Sequential(*list(resnet18(pretrained=False).children())[:-3])
        with torch.no_grad():
            _, channels, height, width = self.conv_nn(
                torch.zeros(1, CHANNELS, HEIGHT, WIDTH)).shape
        self.steps = int(width)
        self.features = int(channels * height)
        self.linear1 = nn.Linear(self.features, LINEAR_SIZE)
        self.recurrent_layer1 = BlockRNN(LINEAR_SIZE, HIDDEN_SIZE)
        self.recurrent_layer2 = BlockRNN(HIDDEN_SIZE, HIDDEN_SIZE)
        self.linear2 = nn.Linear(HIDDEN_SIZE * 2, CLASSES)

    def forward(self, batch):
        """Nomeroff's forward, unchanged: logits shaped [steps, batch, classes]."""
        batch_size = batch.size(0)
        batch = self.conv_nn(batch)
        batch = batch.permute(0, 3, 1, 2)
        n_channels = batch.size(1)
        batch = batch.reshape(batch_size, n_channels, -1)
        batch = self.linear1(batch)
        batch = self.recurrent_layer1(batch, add_output=True)
        batch = self.recurrent_layer2(batch)
        batch = self.linear2(batch)
        return batch.permute(1, 0, 2)


class Batch1Export(nn.Module):
    """The same computation for one crop with every shape constant, so the ONNX graph carries no
    shape arithmetic for TensorRT 8.2: a fixed reshape, explicit zero LSTM states, and logits as
    [1, steps, classes], which is the same memory as Nomeroff's [steps, 1, classes]."""

    def __init__(self, model):
        super().__init__()
        self.model = model
        self.register_buffer("zero_state", torch.zeros(2, 1, HIDDEN_SIZE))

    def forward(self, batch):
        model = self.model
        batch = model.conv_nn(batch).permute(0, 3, 1, 2).reshape(1, model.steps, model.features)
        batch = model.linear1(batch)
        state = (self.zero_state, self.zero_state)
        batch = model.recurrent_layer1(batch, add_output=True, state=state)
        batch = model.recurrent_layer2(batch, state=state)
        return model.linear2(batch)


def load_state(path):
    checkpoint = torch.load(path, map_location="cpu")
    if isinstance(checkpoint, dict) and isinstance(checkpoint.get("state_dict"), dict):
        return checkpoint["state_dict"]
    raise SystemExit("export_nomeroff_onnx: %s has no state_dict" % path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--checkpoint", required=True, help="Nomeroff kz checkpoint (.ckpt)")
    parser.add_argument("--output", required=True, help="ONNX file to write")
    args = parser.parse_args(argv)

    torch.manual_seed(0)
    model = NomeroffKzOcr()
    model.load_state_dict(load_state(args.checkpoint), strict=True)
    model.eval()
    export = Batch1Export(model).eval()

    with torch.no_grad():
        worst = 0.0
        for _ in range(8):
            image = torch.rand(1, CHANNELS, HEIGHT, WIDTH)
            reference = model(image).permute(1, 0, 2)
            worst = max(worst, float((export(image) - reference).abs().max()))
        if worst > 1e-4:
            raise SystemExit("export_nomeroff_onnx: export graph differs from Nomeroff by %g" % worst)

        directory = os.path.dirname(os.path.abspath(args.output))
        if not os.path.isdir(directory):
            os.makedirs(directory)
        handle, temporary = tempfile.mkstemp(prefix=".nomeroff-", suffix=".onnx", dir=directory)
        os.close(handle)
        try:
            torch.onnx.export(export, torch.rand(1, CHANNELS, HEIGHT, WIDTH), temporary,
                              input_names=["image"], output_names=["logits"],
                              opset_version=11, do_constant_folding=True)
            os.chmod(temporary, 0o644)
            os.replace(temporary, args.output)
        except BaseException:
            if os.path.exists(temporary):
                os.unlink(temporary)
            raise
    print("export_nomeroff_onnx: wrote %s (input 1x%dx%dx%d, logits 1x%dx%d, max diff %.2g)" % (
        args.output, CHANNELS, HEIGHT, WIDTH, model.steps, CLASSES, worst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
