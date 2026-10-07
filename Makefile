SHELL := /bin/bash

.PHONY: jetson-all docker-build jetson-check check run project run-all benchmark-1 benchmark-4 \
	research ocr-benchmark

# The bundled clips: Japanese demo (BR45IL), Kazakhstan plate (152JTA02), unlabelled dashcam.
VIDEOS ?= video/car.mp4 video/parking.mp4 video/parking2.mp4

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

# `make run` once per clip in VIDEOS, one after another, with the same OCR.
# Example: PROJECT_OCR=fast_plate_ocr make run-all
run-all:
	@for video in $(VIDEOS); do \
		echo "==== $$video (OCR: $(or $(PROJECT_OCR),easyocr_onnx))"; \
		PROJECT_OCR=$(or $(PROJECT_OCR),easyocr_onnx) ./tools/jetson_docker.sh run \
			--source "$$video" $(RUN_ARGS) || exit $$?; \
	done

benchmark-1:
	./tools/jetson_docker.sh benchmark --research --research-streams 1 $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),)

benchmark-4:
	./tools/jetson_docker.sh benchmark --research --research-streams 4 $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),)

research:
	./tools/jetson_docker.sh benchmark --research $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),)

# Sequential OCR comparison on all three clips: one engine at a time on identical crops, then
# every frame of each clip. Prints the comparison table, ranking and total time; writes Markdown
# and JSON. Limit the engines with OCR_ENGINES, for example:
#   make ocr-benchmark OCR_ENGINES="easyocr_onnx fast_plate_ocr"
ocr-benchmark:
	./tools/jetson_docker.sh benchmark --ocr-benchmark $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),) \
		$(foreach engine,$(OCR_ENGINES),--ocr-engine $(engine))
