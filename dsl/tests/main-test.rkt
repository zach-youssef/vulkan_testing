#lang racket/base
;; Programs' mains: their command lines, windows, clocks and captures, and what C++ gives them

(require racket/list
         racket/string
         rackunit
         "../compile.rkt"
         "util.rkt")

(define (generated text suffix)
  (define o (for/first ([o (in-list (compile-text text))] #:when (string-suffix? (output-path o) suffix)) o))
  (and o (output-text o)))

;; A headless painter: options set its parameters, and it captures to a path the user gives
(define painter
  (string-append
   "(compute-material Fill (workgroup 8 8) (bindings (canvas (storage-image write))) (push (level float))\n"
   "  (compute (store! canvas (ivec2 globalInvocationId.xy) (vec4 level 0.0 0.0 1.0))))\n"
   "(compute-material Count (workgroup 1) (bindings (counts (buffer-array uint))) (compute (write! (at counts 0) 1)))\n"
   "(graph Painting\n"
   "  (parameter level float)\n"
   "  (parameter side uint)\n"
   "  (input file path)\n"
   "  (input seed (data uint))\n"
   "  (image-file source file #:color srgb)\n"
   "  (image canvas rgba8 (fixed side side) #:frames shared)\n"
   "  (buffer counts uint 1 #:frames shared)\n"
   "  (material fill Fill canvas #:push ((level level)) #:invocations (side side))\n"
   "  (material count Count counts #:invocations (1))\n"
   "  (dispatch paint fill)\n"
   "  (dispatch counting count)\n"
   "  (readback result canvas #:when (> side 1))\n"
   "  (readback countsRead counts))\n"
   "(app Painter\n"
   "  (parameter level float)\n"
   "  (parameter bright bool)\n"
   "  (parameter side uint)\n"
   "  (parameter picture path)\n"
   "  (parameter destination path)\n"
   "  (parameter scale float)\n"
   "  (extern (note [x : float]) : float)\n"
   "  (run Painting (level (if bright (note 1.0) (* level scale))) (side side) (file picture)))\n"))

(define painter-main
  (string-append
   "(main Painter\n"
   "  (about \"Paints a square\")\n"
   "  (option level \"--level\" \"the red level\" #:default 0.5 #:range (0.0 1.0) #:given bright)\n"
   "  (option side \"--side\" \"the square's side\" #:default 4)\n"
   "  (positional picture \"an image\")\n"
   "  (positional destination \"where the square goes\")\n"
   "  (capture result #:to destination))\n"))

(test-case "Options set the app's parameters, and C++ gives what they don't"
  (define text (string-append painter painter-main))
  (define main (generated text ".zgal.main.cpp"))
  (check-true (string-contains? main "#include \"zgal/Main.h\""))
  (check-false (string-contains? main "zgal/Window.h"))
  (check-true (string-contains? main "inputs.level = 0.5f;"))
  (check-true (string-contains? main "inputs.side = 4u;"))
  (check-true (string-contains? main "commandLine.option(\"--level\", \"<number>\", \"the red level (0.0 to 1.0, default 0.5)\""))
  (check-true (string-contains? main "inputs.bright = true;"))
  (check-true (string-contains? main "zgal::parseNumber<decltype(inputs.side)>(text, inputs.side);"))
  (check-true (string-contains? main "commandLine.positional(\"<picture>\", \"an image\""))
  (check-true (string-contains? main "inputs.picture = zgal::commandLinePath(text);"))
  ;; The capture goes where the user said, from an optional readback
  (check-true (string-contains? main "launch.capture = inputs.destination;"))
  (check-true (string-contains? main "zgal::captureReadback(app.renderGraph(), zgal::require(app.ids().nodes.result, \"Painting\", \"result\"))"))
  (check-true (string-contains? main "return zgal::runHeadless<App>("))
  ;; C++ gives the parameter no option sets, the extern and the data input, through mainInputs
  (check-true (string-contains? main "TRY_ASSIGN(auto given, test::mainInputs(context));"))
  (check-true (string-contains? main "inputs.scale = std::move(given.scale);"))
  (check-true (string-contains? main "inputs.note = std::move(given.note);"))
  (check-true (string-contains? main "inputs.seed = std::move(given.seed);"))
  (define header (generated text ".zgal.h"))
  (check-true (string-contains? header "struct MainInputs {"))
  (check-true (string-contains? header "Result<MainInputs> mainInputs(VulkanContext& context);"))
  ;; Without a main, there's neither
  (check-false (generated painter ".zgal.main.cpp"))
  (check-false (string-contains? (generated painter ".zgal.h") "MainInputs")))

(define clocked
  (string-append
   "(compute-material Tick (workgroup 1) (bindings (canvas (storage-image write))) (compute (store! canvas (ivec2 0) (vec4 1.0))))\n"
   "(graph Ticks\n"
   "  (parameter capturing bool)\n"
   "  (input shade (per-frame float))\n"
   "  (image canvas rgba8 (surface 1.0) #:frames shared)\n"
   "  (material tick Tick canvas #:invocations (1))\n"
   "  (dispatch ticking tick)\n"
   "  (readback shot canvas #:when capturing))\n"
   "(graph Warmup (image w rgba8 (fixed 1 1) #:frames shared) (material warm Tick (canvas w) #:invocations (1)) (dispatch warming warm))\n"
   "(app Clock\n"
   "  (state (t float 0.0))\n"
   "  (on (frame dt) (when (= frameNumber 3) (quit!)) (with state #:t (+ state.t dt)))\n"
   "  (run Warmup #:when (= frameNumber 0))\n"
   "  (run Ticks (capturing capture) (shade state.t)))\n"))

(test-case "Programs with a window choose the app's clock and how it's run, and need no C++"
  (define main (generated (string-append clocked
                                         "(main Clock (window \"Clock\" 640 360) (timestep #:max 0.25 #:fixed 1/60)\n"
                                         "  (capture shot) (controls \"Esc  quit\"))\n")
                          ".zgal.main.cpp"))
  (check-true (string-contains? main "#include \"zgal/Window.h\""))
  (check-true (string-contains? main ".window = zgal::MainSpec::Window{\"Clock\", 640, 360},"))
  (check-true (string-contains? main ".captureOption = true,"))
  (check-true (string-contains? main "\"Esc  quit\","))
  (check-true (string-contains? main "inputs.timestep = launch.timestep(0.25f, 0.016666666666666666f);"))
  (check-true (string-contains? main "inputs.mode = launch.mode();"))
  (check-false (string-contains? main "mainInputs"))
  ;; A graph each
  (check-true (string-contains? main "graphs.warmup = &newGraph();"))
  (check-true (string-contains? main "return zgal::captureReadback(app.renderGraph(), zgal::require(app.ticksIds().nodes.shot, \"Ticks\", \"shot\"));"))
  (check-true (string-contains? main "return zgal::runWindowed<App>(")))

(test-case "Mains are checked against their app"
  (define (with-main m) (string-append painter m))
  (check-zgal-error (with-main "(main Nobody)") 'unknown-name "Nobody isn't an app")
  (check-zgal-error (with-main "(main Painter (flag level \"--level\" \"x\"))") 'type-mismatch "a flag sets a bool parameter")
  (check-zgal-error (with-main "(main Painter (option bright \"--bright\" \"x\" #:default 1))") 'type-mismatch "flags set bools")
  (check-zgal-error (with-main "(main Painter (option level \"--level\" \"x\"))") 'bad-option "needs a #:default")
  (check-zgal-error (with-main "(main Painter (option level \"--frames\" \"x\" #:default 1.0))") 'duplicate-name "built-in option")
  (check-zgal-error (with-main "(main Painter (option level \"--a\" \"x\" #:default 1.0) (option scale \"--a\" \"y\" #:default 1.0))")
                    'duplicate-name "already an option")
  (check-zgal-error (with-main "(main Painter (option level \"--a\" \"x\" #:default 1.0) (positional level \"y\"))")
                    'duplicate-name "level")
  (check-zgal-error (with-main "(main Painter (option side \"--side\" \"x\" #:default 1.5))") 'type-mismatch "the default is a uint")
  (check-zgal-error (with-main "(main Painter (option level \"--level\" \"x\" #:default 2.0 #:range (0.0 1.0)))") 'bad-option "outside the range")
  (check-zgal-error (with-main "(main Painter (option level \"blur\" \"x\" #:default 1.0))") 'malformed "two dashes")
  (check-zgal-error (with-main "(main Painter (capture countsRead))") 'type-mismatch "reads a buffer")
  (check-zgal-error (with-main "(main Painter (capture paint))") 'type-mismatch "not a readback")
  (check-zgal-error (with-main "(main Painter (capture result #:to destination))") 'bad-option "an option sets")
  (check-zgal-error (with-main "(main Painter (option level \"--level\" \"x\" #:default 0.5) (capture result #:to level))")
                    'type-mismatch "path parameter")
  (check-zgal-error (with-main "(main Painter (timestep #:max 1.0 #:fixed 1.0))") 'bad-option "no frame handler")
  (check-zgal-error (with-main "(main Painter (window \"x\" 0 10))") 'type-mismatch "whole pixels")
  (check-zgal-error (with-main "(main Painter (title \"x\"))") 'unknown-clause "title")
  (check-zgal-error (with-main "(main Painter) (main Painter)") 'duplicate-clause "one main")
  (check-zgal-error (string-append clocked "(main Clock)") 'missing-clause "chooses its clock"))
