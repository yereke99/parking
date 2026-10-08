SHELL := /bin/bash

.PHONY: docker-build check run run-videos prepare-videos camera service bench shell build test

# Test clip and live camera for `make run`, `make camera` and `make service`.
VIDEO ?= video/parking.mp4
CAMERA ?=
CAMERA_ID ?= gate-01

# ---- Jetson Nano (Docker) ----------------------------------------------------------------------

# Build the image: C++ binaries, the ONNX Runtime fallback, Nomeroff's Kazakhstan OCR model.
docker-build:
	./tools/jetson_docker.sh build

# List the backends, load both models on TensorRT and cache their engines.
check:
	./tools/jetson_docker.sh check

# Replay the test clip once and print the recognised plates.
run:
	./tools/jetson_docker.sh run --source "$(VIDEO)" $(RUN_ARGS)

# Replay tools/video-list.txt one clip at a time, saving events, logs and a plate summary.
# VIDEO_ARGS='--native' uses the existing local build and development config.
run-videos:
	python3 tools/run_videos.py $(VIDEO_ARGS)

# Optional, on a machine with FFmpeg: iPhone HEVC -> upright 8-bit H.264 copies.
prepare-videos:
	bash tools/prepare_iphone_videos.sh

# Live camera in the foreground (Ctrl+C stops it), for example:
#   make camera CAMERA='rtsp://user:password@192.168.1.64:554/Streaming/Channels/101'
#   make camera CAMERA=0        (first V4L2/USB camera)
# Every event is also appended to var/events.jsonl.
camera:
	@if [[ -z "$(CAMERA)" ]]; then \
		echo "usage: make camera CAMERA=rtsp://user:password@host:554/stream"; exit 2; \
	fi
	./tools/jetson_docker.sh run --source "$(CAMERA)" --camera-id "$(CAMERA_ID)" \
		--events-file var/events.jsonl $(RUN_ARGS)

# The same camera as a systemd service: starts at boot, restarts after any failure (asks for
# sudo). Runs `make check` first.
service:
	@if [[ -z "$(CAMERA)" ]]; then \
		echo "usage: make service CAMERA=rtsp://user:password@host:554/stream"; exit 2; \
	fi
	./tools/install_service.sh "$(CAMERA)" "$(CAMERA_ID)"

# FPS and latency on the test clip.
bench:
	./tools/jetson_docker.sh bench --video "$(VIDEO)" $(BENCH_ARGS)

shell:
	./tools/jetson_docker.sh shell

# ---- Development machine (native CMake build) --------------------------------------------------

build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS)
	cmake --build build -j

test: build
	cd build && ctest --output-on-failure
