SHELL := /bin/bash

.PHONY: jetson-all docker-build jetson-check run project benchmark-1 benchmark-4 research

# Complete reproducible Jetson workflow. The research target prints the comparison table and the
# exact JSON/Markdown result paths when it finishes.
jetson-all:
	$(MAKE) docker-build
	$(MAKE) jetson-check
	$(MAKE) research

docker-build:
	./tools/jetson_docker.sh build

jetson-check:
	./tools/jetson_docker.sh check

# Full ANPR project: native TensorRT detector + CUDA EasyOCR by default.
# Example: make run RUN_ARGS='--source rtsp://user:pass@camera/stream'
run project:
	PROJECT_OCR=$(or $(PROJECT_OCR),easyocr) ./tools/jetson_docker.sh run $(RUN_ARGS)

benchmark-1:
	./tools/jetson_docker.sh benchmark --research --research-streams 1 $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),)

benchmark-4:
	./tools/jetson_docker.sh benchmark --research --research-streams 4 $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),)

research:
	./tools/jetson_docker.sh benchmark --research $(if $(MAX_FRAMES),--max-frames $(MAX_FRAMES),)
