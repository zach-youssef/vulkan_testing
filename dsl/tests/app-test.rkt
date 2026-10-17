#lang racket/base
;; Apps: their state, handlers and graphs, and what they generate

(require racket/string
         rackunit
         "../compile.rkt"
         "util.rkt")

(define (header text)
  (output-text (for/first ([o (in-list (compile-text text))] #:when (string-suffix? (output-path o) ".zgal.h")) o)))

(define counter
  (string-append
   "(app Counter\n"
   "  (parameter increment uint)\n"
   "  (state (count uint 0) (paused bool #f))\n"
   "  (on (key-pressed up equal) (with state #:count (+ state.count increment)))\n"
   "  (on (key-pressed space) (print! \"paused: \" (not state.paused)) (with state #:paused (not state.paused)))\n"
   "  (on (key-pressed escape) (quit!))\n"
   "  (on (frame dt) (if state.paused state (with state #:count (+ state.count 1)))))\n"))

(test-case "Handlers compute the next state, with effects; a handler of only effects keeps it"
  (define h (header counter))
  (check-true (string-contains? h "case zgal::Key::Up:\n"))
  (check-true (string-contains? h "case zgal::Key::Equal:\n"))
  (check-true (string-contains? h "std::cout << \"paused: \" << (!state.paused ? \"true\" : \"false\") << std::endl;"))
  (check-true (string-contains? h "quit_ = true;"))
  (check-true (string-contains? h "zgalUpdated1.count = state.count + inputs_.increment;"))
  ;; The frame handler runs on the app's clock, which C++ must choose
  (check-true (string-contains? h "zgal::Timestep timestep;"))
  (check-true (string-contains? h "state_ = on3(state_, inputs_.timestep.next());")))

(test-case "Handlers' results are the state"
  (check-zgal-error "(app A (state (x float 0.0)) (on (key-pressed a) 1.0))" 'type-mismatch "the next state")
  (check-zgal-error "(app A (state (x float 0.0)) (on (frame dt) (with state #:x dt) (quit!)))" 'type-mismatch "only the last expression"))

(test-case "Events are handled once, by keys and buttons apps know"
  (check-zgal-error "(app A (on (key-pressed a b) (quit!)) (on (key-pressed b) (quit!)))" 'duplicate-name "key-pressed b is already handled")
  (check-zgal-error "(app A (on (key-pressed hyper) (quit!)))" 'malformed "hyper isn't a key")
  (check-zgal-error "(app A (on (mouse-pressed thumb p) (quit!)))" 'malformed "left, right or middle")
  (check-zgal-error "(app A (on (wheel p) (quit!)))" 'malformed "expected an event"))

(test-case "Apps' effects are only in apps"
  (check-zgal-error "(define (f [x : float]) : float (begin (print! x) x))" 'bad-effect "only app handlers")
  (check-zgal-error "(compute-material C (workgroup 1) (compute (quit!)))" 'bad-effect "only app handlers"))

(define graph
  (string-append
   "(struct Tint (color vec4))\n"
   "(graphics-material Flat (bindings (tint (buffer Tint))) (outputs (color vec4))\n"
   "  (vertex (results #:position (vec4 0.0))) (fragment tint.color))\n"
   "(graph G\n"
   "  (parameter scale float)\n"
   "  (input tint (per-frame Tint))\n"
   "  (input work cpu)\n"
   "  (uniform tints Tint tint)\n"
   "  (image i rgba8 (fixed 4 4) #:frames shared)\n"
   "  (target t (color i))\n"
   "  (material plain Flat (tint tints))\n"
   "  (draw drawFlat t plain 3)\n"
   "  (cpu run work))\n"))

(test-case "Apps compute their graph's parameters once and per-frame inputs from the state; C++ gives the rest"
  (define h (header (string-append graph
                                   "(app A (parameter level float) (state (on bool #t))\n"
                                   "  (run G (scale (* level 2.0)) (tint (Tint (vec4 level)))\n"
                                   "    #:enabled ((drawFlat state.on))))\n")))
  (check-true (string-contains? h ".scale = app->parameter_scale(),"))
  (check-true (string-contains? h ".tint = [self = app.get()](const zgal::FrameInfo&) { return self->input_tint(); },"))
  (check-true (string-contains? h ".work = std::move(app->inputs_.work),"))
  (check-true (string-contains? h "RenderGraph::RunFn work;"))
  (check-true (string-contains? h "graph0_->setEnabled(ids0_->nodes.drawFlat, enabled_drawFlat());"))
  ;; Parameters are computed when the app is created, before there's a state
  (check-zgal-error (string-append graph "(app A (state (s float 1.0)) (run G (scale state.s)))") 'unknown-name "state isn't declared")
  (check-zgal-error (string-append graph "(app A (run G (work 1)))") 'type-mismatch "work is a cpu input, which C++ gives")
  ;; What C++ gives shares the app's inputs with its parameters
  (check-zgal-error (string-append graph "(app A (parameter work uint) (run G (scale 1.0) (tint (Tint (vec4 1.0)))))")
                    'duplicate-name "work")
  (check-zgal-error (string-append graph "(app A (run G #:enabled ((nothing #t))))") 'unknown-name "graph G has no node nothing"))
