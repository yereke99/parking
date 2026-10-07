SHELL := /bin/bash

.PHONY: jetson-all docker-build jetson-check check run project benchmark-1 benchmark-4 research \
	ocr-benchmark

# Complete reproducible Jetson workflow. The research target prints the comparison table and the
# exact JSON/Markdown result paths when it finishes.
jetson-all:
	$(MAKE) docker-build
	$(MAKE) jetson-check
	$(MAKE) research

docker-build:
	./tools/jetson_docker.sh build

jetson-check check:
	./tools/jetson_docker.sh check

# Full ANPR project: TensorRT detector + EasyOCR on TensorRT in the same process by default.
# PROJECT_OCR=easyocr selects the PyTorch CUDA worker instead.
# Example: make run RUN_ARGS='--source rtsp://user:pass@camera/stream'
run project:
	PROJECT_OCR=$(or $(PROJECT_OCR),easyocr_onnx) ./tools/jetson_docker.sh run $(RUN_ARGS)

benchmark-1:
	./tools/jetson_docker.sh benchmark --research --research-streams 1 $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),)

benchmark-4:
	./tools/jetson_docker.sh benchmark --research --research-streams 4 $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),)

research:
	./tools/jetson_docker.sh benchmark --research $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),)

# Sequential OCR comparison: one engine at a time on identical crops, then every frame of each
# clip. Prints the comparison table, ranking and total time; writes Markdown and JSON.
ocr-benchmark:
	./tools/jetson_docker.sh benchmark --ocr-benchmark $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),)
