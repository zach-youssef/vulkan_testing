# Compiler: clang, the one tested (Apple's c++ on macOS, clang++ on Linux), unless CXX is given in
# the environment or on the command line (make CXX=g++). make's own default (g++), or none under
# make -R, counts as not given.
ifneq ($(filter default undefined,$(origin CXX)),)
CXX = $(if $(filter Darwin,$(ZGAL_OS)),c++,clang++)
endif

CFLAGS = -std=c++20
CXXFLAGS = $(CFLAGS)

# Links add the platform's library paths (zgal.mk's ZGAL_LDFLAGS) to LDFLAGS
LDFLAGS =

LDLIBS = -lvulkan
# Only the app layer (include/app) uses GLFW
GLFW_LDLIBS = -lglfw

OUT = build
# The wrapper's include paths (zgal.mk's ZGAL_CFLAGS: include/, the third-party libraries and the
# platform's)
CFLAGS += $(ZGAL_CFLAGS)

HEADERS = $(ZGAL_HEADERS)

# Shaders include the library's GLSL (e.g. bindless.glsl)
GLSL_INCLUDES = $(ZGAL_GLSL_INCLUDES)
GLSLC_FLAGS = $(ZGAL_GLSLC_FLAGS)

# ---------------------------------------------------------------------------
# ZGAL (dsl/): the shader interface language. Its compiler, in Racket, generates a C++ header and
# complete shaders from a .zgal file. make docs builds its documentation. zgal.mk has the rules
# building its programs, which other projects include too.
# ---------------------------------------------------------------------------
ZGAL_ROOT = .
include zgal.mk
DSL_DIR = $(ZGAL_DSL_DIR)

# ---------------------------------------------------------------------------
# Examples
#   make            - build the simple example into build/examples/simple
# ---------------------------------------------------------------------------
EXAMPLES_DIR = examples
EXAMPLES_OUT = $(OUT)/examples

SIMPLE_DIR = $(EXAMPLES_DIR)/simple
SIMPLE_OUT = $(EXAMPLES_OUT)/simple
# The shaders are in simple.zgal; the graph is built in C++
SIMPLE_GEN = $(SIMPLE_OUT)/generated
SIMPLE_ZGAL_STAMP = $(SIMPLE_GEN)/simple.zgal.stamp
SIMPLE_CFLAGS = $(CFLAGS) -I$(SIMPLE_GEN) \
                -DSIMPLE_EXAMPLE_TEXTURE_DIR='"$(abspath $(SIMPLE_DIR)/textures)"'

$(eval $(call zgal-rule,$(SIMPLE_DIR)/simple.zgal,$(SIMPLE_GEN),$(SIMPLE_OUT)/shaders))

$(SIMPLE_OUT)/simple_example.o : $(SIMPLE_DIR)/simple_example.cpp $(SIMPLE_ZGAL_STAMP) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) -c -o $@ $< $(SIMPLE_CFLAGS)

$(SIMPLE_OUT)/simple_example: $(SIMPLE_OUT)/simple_example.o
	$(CXX) $^ -o $@ $(LDFLAGS) $(ZGAL_LDFLAGS) $(LDLIBS) $(GLFW_LDLIBS)

# ---------------------------------------------------------------------------
# Example: image_filter
#   A headless command-line GPU image filter (no GLFW)
#   make test-example-image_filter - smoke test with validation, plus golden image checks
# ---------------------------------------------------------------------------
IMAGE_FILTER_DIR = $(EXAMPLES_DIR)/image_filter
IMAGE_FILTER_OUT = $(EXAMPLES_OUT)/image_filter
IMAGE_FILTER_BIN = $(IMAGE_FILTER_OUT)/image_filter
# Everything is in image_filter.zgal, main too
IMAGE_FILTER_ZGAL_STAMP = $(IMAGE_FILTER_OUT)/generated/image_filter.zgal.stamp
IMAGE_FILTER_TEST_DIR = $(IMAGE_FILTER_DIR)/tests
IMAGE_FILTER_TEST_OUT = $(IMAGE_FILTER_OUT)/tests
IMAGE_FILTER_DIFF = $(IMAGE_FILTER_TEST_OUT)/image_diff

$(eval $(call zgal-program,$(IMAGE_FILTER_DIR)/image_filter.zgal,$(IMAGE_FILTER_OUT),image_filter,))

$(IMAGE_FILTER_DIFF): $(IMAGE_FILTER_TEST_DIR)/image_diff.cpp
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $< $(CFLAGS)

# Every pass on the simple example's texture with validation on (any message fails the run), then
# golden checks on a small input: without filters the output must match the input (colour spaces
# round trip), blur plus edges must match the approved output within a tolerance, and so must blur,
# edges and threshold (but for the few pixels next to the threshold). To approve a new output, run
# its command below and copy its output over the approved image.
test-example-image_filter: $(IMAGE_FILTER_BIN) $(IMAGE_FILTER_DIFF)
	@mkdir -p $(IMAGE_FILTER_TEST_OUT)
	./$(IMAGE_FILTER_BIN) $(SIMPLE_DIR)/textures/texture.jpg $(IMAGE_FILTER_TEST_OUT)/all_passes.png \
		--blur 2 --edges --threshold 0.15 --validation
	./$(IMAGE_FILTER_BIN) $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_TEST_OUT)/identity.png --validation
	./$(IMAGE_FILTER_DIFF) $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_TEST_OUT)/identity.png 1
	./$(IMAGE_FILTER_BIN) $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_TEST_OUT)/blur_edges.png \
		--blur 2 --edges --validation
	./$(IMAGE_FILTER_DIFF) $(IMAGE_FILTER_TEST_DIR)/blur_edges.approved.png $(IMAGE_FILTER_TEST_OUT)/blur_edges.png 2
	./$(IMAGE_FILTER_BIN) $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_TEST_OUT)/threshold.png \
		--blur 2 --edges --threshold 0.15 --validation
	./$(IMAGE_FILTER_DIFF) $(IMAGE_FILTER_TEST_DIR)/threshold.approved.png $(IMAGE_FILTER_TEST_OUT)/threshold.png 0 $(IMAGE_FILTER_THRESHOLD_FLIPS)

# Pixels whose edge magnitude is next to the threshold can come out either side of it on different
# devices: the fraction of the threshold test's pixels that may differ. About twice what lavapipe
# flips (8 of 16384 pixels; none on the Mac, natively or on Dawn), so a real change still fails.
IMAGE_FILTER_THRESHOLD_FLIPS = 0.001

# For the web: build/web/image_filter (runs headless under Node)
IMAGE_FILTER_WEB = $(OUT)/web/image_filter
$(eval $(call zgal-web-program,$(IMAGE_FILTER_DIR)/image_filter.zgal,$(IMAGE_FILTER_WEB)))

# ---------------------------------------------------------------------------
# Example: particles
#   GPU particle simulation drawn in a window
#   make test-example-particles - runs it headless for 120 frames with validation, capturing the last
# ---------------------------------------------------------------------------
PARTICLES_DIR = $(EXAMPLES_DIR)/particles
PARTICLES_OUT = $(EXAMPLES_OUT)/particles
# Everything is in particles.zgal, main too
PARTICLES_ZGAL_STAMP = $(PARTICLES_OUT)/generated/particles.zgal.stamp

$(eval $(call zgal-program,$(PARTICLES_DIR)/particles.zgal,$(PARTICLES_OUT),particles,$(GLFW_LDLIBS)))

test-example-particles: $(PARTICLES_OUT)/particles
	./$< --frames 120 --size 1024x768 --capture $(PARTICLES_OUT)/smoke.png --validation

# For the web: build/web/particles (serve it and open particles.zgal.html, or run it under Node)
PARTICLES_WEB = $(OUT)/web/particles
$(eval $(call zgal-web-program,$(PARTICLES_DIR)/particles.zgal,$(PARTICLES_WEB)))

# ---------------------------------------------------------------------------
# Example: scene
#   A lit 3D scene: shadow map, forward PBR into HDR, tonemap
#   make test-example-scene - headless smoke test (validation on, fails on any message)
# ---------------------------------------------------------------------------
SCENE_DIR = $(EXAMPLES_DIR)/scene
SCENE_OUT = $(EXAMPLES_OUT)/scene
# Everything is in scene.zgal, main too, and the scene is assets/scene.gltf (make_scene.py)
SCENE_ZGAL_STAMP = $(SCENE_OUT)/generated/scene.zgal.stamp

$(eval $(call zgal-program,$(SCENE_DIR)/scene.zgal,$(SCENE_OUT),scene_example,$(GLFW_LDLIBS)))

# Renders frames headless, as the app scripts them (shadows off for a quarter, resized halfway), and
# captures the last one
test-example-scene: $(SCENE_OUT)/scene_example
	./$< --frames 60 --size 640x360 --capture $(SCENE_OUT)/smoke.png --validation

# For the web: build/web/scene (serve it and open scene.zgal.html, or run it under Node)
SCENE_WEB = $(OUT)/web/scene
$(eval $(call zgal-web-program,$(SCENE_DIR)/scene.zgal,$(SCENE_WEB)))

# All the examples' smoke tests
test-examples: test-example-image_filter test-example-particles test-example-scene test-main test-bench

examples: $(SIMPLE_OUT)/simple_example $(IMAGE_FILTER_BIN) $(PARTICLES_OUT)/particles $(SCENE_OUT)/scene_example

shaders: $(SIMPLE_ZGAL_STAMP) $(IMAGE_FILTER_ZGAL_STAMP) $(PARTICLES_ZGAL_STAMP) $(SCENE_ZGAL_STAMP)

.DEFAULT_GOAL := examples

# ---------------------------------------------------------------------------
# Tests
#   make test-cpu   - unit tests that do not need a Vulkan device
#   make test-gpu   - tests that run headless on a Vulkan device (no window)
#   make test       - all tests
#   make test-examples - every example's smoke test (headless, with validation)
#   make approve-screenshots - accept the screenshots from the last failed GPU test run
# Arguments can be forwarded to doctest, e.g. make test-cpu TEST_ARGS='-tc="Handle*"'
# ---------------------------------------------------------------------------
TEST_DIR = tests
TEST_OUT = $(OUT)/tests
TEST_SHADER_OUT = $(TEST_OUT)/shaders
# The GPU tests' ZGAL interfaces (tests/gpu/zgal), generated like an example's
TEST_GEN = $(TEST_OUT)/generated
TEST_ZGAL_DIR = $(TEST_DIR)/gpu/zgal
TEST_ZGAL_STAMP = $(TEST_GEN)/interfaces.zgal.stamp
TEST_CFLAGS = $(CFLAGS) -Ithird_party -Ithird_party/approvaltests -Ithird_party/flip -I$(TEST_DIR) -I$(TEST_GEN) \
              -DTEST_TEXTURE_DIR='"$(abspath $(SIMPLE_DIR)/textures)"' \
              -DTEST_ASSET_DIR='"$(abspath $(TEST_DIR)/gpu/assets)"'
TEST_HEADERS = $(HEADERS) $(wildcard $(TEST_DIR)/*/*.h)

CPU_TEST_SRCS = $(wildcard $(TEST_DIR)/cpu/*.cpp)
CPU_TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp,$(TEST_OUT)/%.o,$(CPU_TEST_SRCS))

GPU_TEST_SRCS = $(wildcard $(TEST_DIR)/gpu/*.cpp)
GPU_TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp,$(TEST_OUT)/%.o,$(GPU_TEST_SRCS))

# SPIR-V used by the GPU tests, compiled to $(TEST_SHADER_OUT)/<name>.<stage>.spv: test-only shaders,
# and those of the tests' ZGAL file (with the stamp, as its shaders' names aren't files)
GPU_TEST_SHADER_SRCS = $(wildcard $(TEST_DIR)/gpu/shaders/*)
GPU_TEST_SHADERS = $(patsubst %,$(TEST_SHADER_OUT)/%.spv,$(notdir $(GPU_TEST_SHADER_SRCS))) $(TEST_ZGAL_STAMP)

$(eval $(call zgal-rule,$(TEST_ZGAL_DIR)/interfaces.zgal,$(TEST_GEN),$(TEST_SHADER_OUT)))

$(TEST_OUT)/gpu/ZgalTest.o: $(TEST_ZGAL_STAMP)

$(TEST_SHADER_OUT)/%.spv : $(TEST_DIR)/gpu/shaders/% $(GLSL_INCLUDES)
	@mkdir -p $(dir $@)
	glslc $(GLSLC_FLAGS) $< -o $@

# Compiled from absolute paths so __FILE__ is absolute: ApprovalTests uses it to find approved images
$(TEST_OUT)/%.o : $(TEST_DIR)/%.cpp $(TEST_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) -c -o $@ $(abspath $<) $(TEST_CFLAGS)

# The CPU tests reflect on compiled test shaders
$(TEST_OUT)/cpu_tests: $(CPU_TEST_OBJS) | $(GPU_TEST_SHADERS)
	$(CXX) $^ -o $@ $(LDFLAGS) $(ZGAL_LDFLAGS) $(LDLIBS)

# The GPU tests cover VulkanApp, which uses GLFW (though only headless)
$(TEST_OUT)/gpu_tests: $(GPU_TEST_OBJS) | $(GPU_TEST_SHADERS)
	$(CXX) $^ -o $@ $(LDFLAGS) $(ZGAL_LDFLAGS) $(LDLIBS) $(GLFW_LDLIBS)

test-cpu: $(TEST_OUT)/cpu_tests
	./$< $(TEST_ARGS)

test-gpu: $(TEST_OUT)/gpu_tests
	./$< $(TEST_ARGS)

# A program with a generated main and C++ inputs (tests/main): its command line, exit statuses,
# hook and capture, with validation. Part of the examples' smoke tests, as it's a program.
MAIN_TEST_OUT = $(TEST_OUT)/main
MAIN_TEST_BIN = $(MAIN_TEST_OUT)/painter

$(eval $(call zgal-program,$(TEST_DIR)/main/painter.zgal,$(MAIN_TEST_OUT),painter,))

# The same program for the web, its TypeScript giving the inputs (painter.inputs.ts); make test-web
# runs it as test-main runs the native one
MAIN_TEST_WEB = $(OUT)/web/main

$(eval $(call zgal-web-program,$(TEST_DIR)/main/painter.zgal,$(MAIN_TEST_WEB)))

test-main: $(MAIN_TEST_BIN) $(IMAGE_FILTER_DIFF)
	./$(MAIN_TEST_BIN) --help > /dev/null
	./$(MAIN_TEST_BIN) > /dev/null 2>&1; test $$? -eq 2
	./$(MAIN_TEST_BIN) $(TEST_DIR)/gpu/assets/quadrants.png $(MAIN_TEST_OUT)/out.png --level 2 > /dev/null 2>&1; test $$? -eq 2
	./$(MAIN_TEST_BIN) $(TEST_DIR)/gpu/assets/missing.png $(MAIN_TEST_OUT)/out.png > /dev/null 2>&1; test $$? -eq 1
	@rm -f $(MAIN_TEST_OUT)/out.png
	./$(MAIN_TEST_BIN) $(TEST_DIR)/gpu/assets/quadrants.png $(MAIN_TEST_OUT)/out.png --level 0.5 --side 6 --frames 3 --validation
	./$(IMAGE_FILTER_DIFF) $(TEST_DIR)/main/painted.approved.png $(MAIN_TEST_OUT)/out.png 0
	./$(MAIN_TEST_BIN) $(TEST_DIR)/gpu/assets/quadrants.png $(MAIN_TEST_OUT)/timed.png --frames 2 --timings --validation | grep -q "^painting "

# The benchmark (tools/bench): ZGAL's library kernels and fallbacks at a size the command line gives.
#   make bench                         - builds build/tools/bench/bench
#   make run-bench BENCH_ARGS='...'    - runs it with --timings (e.g. BENCH_ARGS='--count 16777216')
#   make test-bench                    - a small run with validation (part of test-examples)
BENCH_OUT = $(OUT)/tools/bench
BENCH_BIN = $(BENCH_OUT)/bench

$(eval $(call zgal-program,tools/bench/bench.zgal,$(BENCH_OUT),bench,))

bench: $(BENCH_BIN)

run-bench: $(BENCH_BIN)
	./$(BENCH_BIN) --frames 60 --timings $(BENCH_ARGS)

test-bench: $(BENCH_BIN)
	./$(BENCH_BIN) --count 100000 --draws 64 --frames 3 --timings --validation | grep -q "^sortCounted "

# The ZGAL compiler's tests, and its documentation, whose examples are compiled as it's built.
# The tests compile generated code with the build's tools and flags.
DOCS_OUT = $(OUT)/docs
ZGAL_TEST_ENV = ZGAL_GLSLC='glslc' ZGAL_GLSLC_FLAGS='$(GLSLC_FLAGS:-I%=-I$(CURDIR)/%)' \
                 ZGAL_CXX='$(CXX)' ZGAL_CXXFLAGS='$(subst -I./,-I$(CURDIR)/,$(CXXFLAGS))' \
                 ZGAL_NODE='$(NODE)' ZGAL_WEB='$(CURDIR)/$(WEB_DIR)'

# The web runtime (web/): its npm packages (TypeScript, and Dawn, WebGPU for Node, for tests),
# installed by zgal.mk's rule
WEB_DIR = web
WEB_PACKAGES = $(ZGAL_WEB_PACKAGES)

test-dsl: $(ZGAL_COMPILED) $(WEB_PACKAGES) docs
	$(ZGAL_TEST_ENV) $(RACO) test $(DSL_DIR)/tests

# The web runtime's tests: web/tests/*.zgal compiled to TypeScript and WGSL beside a copy of the
# runtime (as an application would have it), type checked, then run on Dawn
WEB_TEST_OUT = $(OUT)/web-tests
# The models web/tests/models.zgal draws, shared with the native tests
MODEL_ASSETS = $(addprefix tests/gpu/assets/,halves.gltf broken.gltf quad.obj quad.mtl quadrants.png)

test-web: $(ZGAL_COMPILED) $(WEB_PACKAGES)
	rm -rf $(WEB_TEST_OUT)
	@mkdir -p $(WEB_TEST_OUT)
	for f in $(WEB_DIR)/tests/*.zgal $(IMAGE_FILTER_DIR)/image_filter.zgal; do $(ZGAL) --web-out $(WEB_TEST_OUT) $$f || exit 1; done
	cp -R $(WEB_DIR)/zgal $(WEB_DIR)/tests $(WEB_DIR)/tools $(WEB_DIR)/package.json $(WEB_DIR)/tsconfig.json $(WEB_TEST_OUT)/
	cp $(MODEL_ASSETS) $(IMAGE_FILTER_TEST_DIR)/input.png $(WEB_TEST_OUT)/tests/assets/
	ln -s $(CURDIR)/$(WEB_DIR)/node_modules $(WEB_TEST_OUT)/node_modules
	cd $(WEB_TEST_OUT) && $(NODE) node_modules/typescript/bin/tsc -p . && $(NODE) --test $(TEST_ARGS) 'tests/*.test.ts'
	$(MAKE) web
	$(NODE) $(PARTICLES_WEB)/particles.zgal.main.js --frames 120 --size 1024x768 --capture $(PARTICLES_WEB)/smoke.png
	$(NODE) $(SCENE_WEB)/scene.zgal.main.js --frames 60 --size 640x360 --capture $(SCENE_WEB)/smoke.png
	$(MAKE) $(MAIN_TEST_WEB)
	$(NODE) $(MAIN_TEST_WEB)/painter.zgal.main.js --help > /dev/null
	$(NODE) $(MAIN_TEST_WEB)/painter.zgal.main.js > /dev/null 2>&1; test $$? -eq 2
	$(NODE) $(MAIN_TEST_WEB)/painter.zgal.main.js $(TEST_DIR)/gpu/assets/missing.png $(MAIN_TEST_WEB)/out.png > /dev/null 2>&1; test $$? -eq 1
	$(NODE) $(MAIN_TEST_WEB)/painter.zgal.main.js $(TEST_DIR)/gpu/assets/quadrants.png $(MAIN_TEST_WEB)/out.png --level 0.5 --side 6 --frames 3 | grep -qx "level 0.25"
	$(NODE) $(WEB_DIR)/tools/image-diff.ts $(TEST_DIR)/main/painted.approved.png $(MAIN_TEST_WEB)/out.png 0
	$(NODE) $(IMAGE_FILTER_WEB)/image_filter.zgal.main.js $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_WEB)/identity.png
	$(NODE) $(WEB_DIR)/tools/image-diff.ts $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_WEB)/identity.png 1
	$(NODE) $(IMAGE_FILTER_WEB)/image_filter.zgal.main.js $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_WEB)/blur_edges.png --blur 2 --edges
	$(NODE) $(WEB_DIR)/tools/image-diff.ts $(IMAGE_FILTER_TEST_DIR)/blur_edges.approved.png $(IMAGE_FILTER_WEB)/blur_edges.png 2
	$(NODE) $(IMAGE_FILTER_WEB)/image_filter.zgal.main.js $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_WEB)/threshold.png --blur 2 --edges --threshold 0.15
	$(NODE) $(WEB_DIR)/tools/image-diff.ts $(IMAGE_FILTER_TEST_DIR)/threshold.approved.png $(IMAGE_FILTER_WEB)/threshold.png 0 $(IMAGE_FILTER_THRESHOLD_FLIPS)
	$(MAKE) test-pages

# The examples' pages, embedding programs (web/tests/mount.html), loading image files
# (web/tests/image-file.html), the compute library (web/tests/kernels.html), indirect work
# (web/tests/indirect.html) and models (web/tests/models.html), with web/tests' graphs built into
# graphs/, in each of BROWSERS: by default only headless Chromium on SwiftShader. Firefox opens
# windows (it has no WebGPU adapter headless, nor any on Linux), so it runs only when asked:
# make test-pages BROWSERS="chromium firefox" (or test-web, or test), as the pre-push check of a
# stack's top does. A browser found nowhere is skipped (the tools exit with 3); one named by CHROMIUM
# or FIREFOX must exist.
BROWSERS = chromium
TEST_PAGES = $(OUT)/web/tests
test-pages: web
	rm -rf $(TEST_PAGES)
	@mkdir -p $(TEST_PAGES)/assets
	cp $(WEB_DIR)/tests/*.html $(TEST_PAGES)/
	cp $(WEB_DIR)/tests/assets/texels.png $(SIMPLE_DIR)/textures/texture.jpg $(MODEL_ASSETS) $(TEST_PAGES)/assets/
	for f in $(WEB_DIR)/tests/*.zgal; do $(ZGAL) --web-out $(TEST_PAGES)/graphs/src $$f || exit 1; done
	cp -R $(WEB_DIR)/zgal $(TEST_PAGES)/graphs/src/zgal
	cp $(WEB_DIR)/program.tsconfig.json $(TEST_PAGES)/graphs/src/tsconfig.json
	echo '{"type": "module"}' > $(TEST_PAGES)/graphs/src/package.json
	ln -sfn $(abspath $(WEB_DIR)/node_modules) $(TEST_PAGES)/graphs/src/node_modules
	$(NODE) $(WEB_DIR)/node_modules/typescript/bin/tsc -p $(TEST_PAGES)/graphs/src --outDir $(TEST_PAGES)/graphs --rootDir $(TEST_PAGES)/graphs/src
	for browser in $(BROWSERS); do \
	  $(NODE) $(WEB_DIR)/tools/$$browser.ts --serve $(PARTICLES_WEB) --keys ' 2' 'particles.zgal.html?frames=60'; \
	  status=$$?; if [ $$status -eq 3 ]; then echo "test-pages: $$browser skipped (not installed)"; continue; fi; \
	  [ $$status -eq 0 ] || exit $$status; \
	  $(NODE) $(WEB_DIR)/tools/$$browser.ts --serve $(SCENE_WEB) 'scene.zgal.html?frames=60' || exit 1; \
	  for page in mount image-file kernels indirect models; do \
	    $(NODE) $(WEB_DIR)/tools/$$browser.ts --serve $(OUT)/web tests/$$page.html || exit 1; \
	  done; \
	done

# The examples whose programs run on the web (simple's app is C++)
web: $(PARTICLES_WEB) $(IMAGE_FILTER_WEB) $(SCENE_WEB)

# build/docs/zgal.html, and zgal.txt for reading in a terminal
docs: $(ZGAL_COMPILED)
	@mkdir -p $(DOCS_OUT)
	$(RACO) scribble --html --dest $(DOCS_OUT) $(DSL_DIR)/docs/zgal.scrbl
	$(RACO) scribble --text --dest $(DOCS_OUT) $(DSL_DIR)/docs/zgal.scrbl

test: test-cpu test-gpu test-dsl test-web

SCREENSHOT_DIR = $(TEST_DIR)/gpu/approved

approve-screenshots:
	@find $(SCREENSHOT_DIR) -name '*.received.png' 2>/dev/null | while read -r received; do \
		approved="$${received%.received.png}.approved.png"; \
		mv "$$received" "$$approved" && echo "Approved $$approved"; \
	done
	@find $(SCREENSHOT_DIR) -name '*.flip.png' -delete 2>/dev/null || true

.PHONY: clean examples shaders test test-cpu test-gpu test-dsl test-web test-pages web docs approve-screenshots test-example-image_filter test-example-particles test-example-scene test-examples test-main bench run-bench test-bench


clean:
	rm -rf $(OUT) $(DSL_DIR)/compiled $(DSL_DIR)/*/compiled
