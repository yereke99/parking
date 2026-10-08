SHELL := /bin/bash

.PHONY: docker-build check run run-videos list-videos prepare-videos camera service bench shell build test
.PHONY: camera-scan camera-check run-cameras camera-status camera-service camera-lan-setup run-all

# Test clip and live camera for `make run`, `make camera` and `make service`.
VIDEO ?= video/parking.mp4
# Clips for `make run-videos` (default: every video file in video/).
VIDEOS ?=
CAMERA ?=
CAMERA_ID ?= gate-01

# Camera mode (docs/CAMERAS.md): the camera config inside the checkout, and the camera passwords
# (gitignored, from config/cameras.env.example).
CAMERA_CONFIG ?= config/cameras.yaml
CAMERA_ENV_FILE ?= config/cameras.env
# For make camera-lan-setup: ADDRESS=192.168.10.5/24, INTERFACE=eth0, LAN_ARGS='--apply'.
LAN_ARGS ?=
ADDRESS ?=
INTERFACE ?=
camera_mode = KZ_ANPR_CAMERA_CONFIG="/workspace/$(CAMERA_CONFIG)" \
	CAMERA_ENV_FILE="$(CAMERA_ENV_FILE)" ./tools/jetson_docker.sh cameras

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

# Replay every video file in video/ one clip at a time, however many there are, saving events,
# logs and a plate summary. VIDEOS='IMG_5667.mp4 IMG_5669.mp4' replays only those clips.
# VIDEO_ARGS='--native' uses the existing local build and development config.
run-videos:
	python3 tools/run_videos.py $(foreach clip,$(VIDEOS),--video "$(clip)") $(VIDEO_ARGS)

# The clips run-videos would replay.
list-videos:
	@python3 tools/run_videos.py --list-videos $(foreach clip,$(VIDEOS),--video "$(clip)") $(VIDEO_ARGS)

# The old name of run-videos.
run-all: run-videos

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

# ---- Hikvision cameras on the PoE switch (docs/CAMERAS.md) --------------------------------------

# Camera LAN, Internet uplink and every camera on the switch; sends no passwords.
camera-scan:
	$(camera_mode) --camera-scan $(RUN_ARGS)

# Per camera: RTSP login, stream path, codec, decoder and frames; exit 0 only if all are READY.
camera-check:
	$(camera_mode) --camera-check $(RUN_ARGS)

# ANPR on every healthy camera in the foreground (Ctrl+C stops it); events also go to
# var/events.jsonl.
run-cameras:
	$(camera_mode) --cameras --events-file var/events.jsonl $(RUN_ARGS)

# Status table of the running camera service, or a quick probe when none is running.
camera-status:
	$(camera_mode) --camera-status $(RUN_ARGS)

# All cameras as the kz-anpr systemd service, replacing `make service` (asks for sudo).
camera-service:
	CAMERA_CONFIG="$(CAMERA_CONFIG)" CAMERA_ENV_FILE="$(CAMERA_ENV_FILE)" \
		./tools/install_service.sh --cameras

# Static camera-LAN address with NetworkManager; a dry run until LAN_ARGS has --apply.
camera-lan-setup:
	./tools/camera_lan_setup.sh $(if $(INTERFACE),--interface "$(INTERFACE)") \
		$(if $(ADDRESS),--address "$(ADDRESS)") $(LAN_ARGS)

# ---- Development machine (native CMake build) --------------------------------------------------

build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS)
	cmake --build build -j

test: build
	cd build && ctest --output-on-failure
