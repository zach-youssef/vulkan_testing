#lang racket/base
;; Reading and checking `main` forms: the program running an app, its command line (options setting
;; the app's parameters, besides the built-in ones), window, clock and capture. The C++ backend
;; (cpp-main.rkt) writes the program's main from it.

(require racket/list
         racket/match
         racket/string
         "ast.rkt"
         "body.rkt"
         "errors.rkt"
         "graph.rkt"
         "syntax.rkt")

(provide parse-main
         builtin-flags
         main-flags
         option-value-type)

;; The options every generated main has (cpp-main.rkt), and those it has with a window or a
;; --capture
(define builtin-flags '("--frames" "--validation" "--plan" "--help" "-h"))
(define window-flags '("--size" "--window"))
(define capture-flags '("--capture"))

;; The built-in options of main m
(define (main-flags m)
  (append builtin-flags
          (if (main-decl-window m) window-flags '())
          (if (and (main-decl-capture m) (not (third (main-decl-capture m)))) capture-flags '())))

;; What an option of parameter p's type takes: 'int 'uint 'float or 'path (flags take bools)
(define (option-value-type p)
  (match (field-type p)
    [(scalar-t (and k (or 'int 'uint 'float))) k]
    [(opaque-t 'path) 'path]
    [_ #f]))

;; stx: the (main App clause ...) form. apps: the file's apps.
(define (parse-main stx apps)
  (define items (syntax->list stx))
  (unless (and items (>= (length items) 2))
    (zgal-error 'malformed stx "expected (main App clause ...)"))
  (define app-stx (second items))
  (define a (or (and (symbol? (syntax-e app-stx))
                     (findf (λ (a) (equal? (app-name a) (symbol->string (syntax-e app-stx)))) apps))
                (zgal-error 'unknown-name app-stx "~a isn't an app declared earlier in the file" (show app-stx))))
  (define clauses (cddr items))
  (define (clauses-of key) (filter (λ (c) (eq? (head-symbol c) key)) clauses))
  (for ([c (in-list clauses)])
    (unless (memq (head-symbol c) '(about window timestep capture controls flag option positional))
      (zgal-error 'unknown-clause c "main takes about, window, timestep, capture, controls, flag, option and positional clauses, not ~a" (show c))))
  (for ([key (in-list '(about window timestep capture controls))])
    (when (> (length (clauses-of key)) 1)
      (zgal-error 'duplicate-clause (second (clauses-of key)) "main has two (~a ...) clauses" key)))
  (define (one key) (match (clauses-of key) ['() #f] [(list c) c]))
  (define (text stx what)
    (unless (string? (syntax-e stx))
      (zgal-error 'type-mismatch stx "~a is a string, not ~a" what (show stx)))
    (syntax-e stx))

  (define about
    (match (one 'about)
      [#f #f]
      [c (match (syntax->list c)
           [(list _ t) (text t "what the program does")]
           [_ (zgal-error 'malformed c "expected (about \"what the program does\")")])]))
  (define window
    (match (one 'window)
      [#f #f]
      [c (match (syntax->list c)
           [(list _ title w h)
            (for ([n (in-list (list w h))])
              (unless (exact-positive-integer? (syntax-e n))
                (zgal-error 'type-mismatch n "a window's size is in whole pixels, not ~a" (show n))))
            (list (text title "the window's title") (syntax-e w) (syntax-e h))]
           [_ (zgal-error 'malformed c "expected (window \"title\" width height)")])]))
  (define frame-handler? (for/or ([h (in-list (app-handlers a))]) (eq? (event-kind (handler-event h)) 'frame)))
  (define timestep
    (match (one 'timestep)
      [#f
       (when frame-handler?
         (zgal-error 'missing-clause stx "app ~a has a frame handler, so main chooses its clock: (timestep #:max seconds #:fixed seconds)"
                     (app-name a)))
       #f]
      [c
       (unless frame-handler?
         (zgal-error 'bad-option c "app ~a has no frame handler, so it has no clock to choose" (app-name a)))
       (define-values (positional options) (split-options (cdr (syntax->list c)) "timestep"))
       (unless (null? positional)
         (zgal-error 'malformed c "expected (timestep #:max seconds #:fixed seconds)"))
       (check-known-options options '(#:max #:fixed) "timestep")
       (define (seconds key what)
         (define v (hash-ref options key (λ () (zgal-error 'bad-option c "timestep needs ~a, ~a" key what))))
         (unless (and (real? (syntax-e v)) (positive? (syntax-e v)))
           (zgal-error 'type-mismatch v "~a is a positive number of seconds, not ~a" key (show v)))
         (exact->inexact (syntax-e v)))
       (cons (seconds '#:max "the longest step interactively (a stall doesn't make the app jump)")
             (seconds '#:fixed "the step with --frames, so runs are deterministic"))]))
  (define controls
    (match (one 'controls)
      [#f '()]
      [c (for/list ([line (in-list (cdr (syntax->list c)))]) (text line "a line of the controls"))]))

  ;; Options set the app's parameters, each at most once (#:given counts)
  (define parameters (app-parameters a))
  (define (parameter-of n-stx)
    (define n (format "~a" (syntax-e n-stx)))
    (or (findf (λ (p) (equal? (field-name p) n)) parameters)
        (zgal-error 'unknown-name n-stx "app ~a has no parameter ~a" (app-name a) n)))
  (define set-by (make-namespace "set by an option"))
  (define flags-seen (make-hash))
  (define (claim-flag! f-stx)
    (define f (syntax-e f-stx))
    (unless (and (string? f) (regexp-match? #px"^--[a-z0-9][a-z0-9-]*$" f))
      (zgal-error 'malformed f-stx "an option is named like \"--blur\": two dashes, then lower-case letters, digits and dashes, not ~a" (show f-stx)))
    (when (member f (append builtin-flags window-flags capture-flags))
      (zgal-error 'duplicate-name f-stx "~a is a built-in option of generated mains" f))
    (when (hash-ref flags-seen f #f)
      (zgal-error 'duplicate-name f-stx "~a is already an option of the program" f))
    (hash-set! flags-seen f #t)
    f)
  (define (number-in stx type what)
    (define v (syntax-e stx))
    (unless (and (real? v) (or (eq? type 'float) (exact-integer? v)) (or (not (eq? type 'uint)) (>= v 0)))
      (zgal-error 'type-mismatch stx "~a is a ~a, not ~a" what type (show stx)))
    v)
  (define (option-of kind c n flag help opts)
    (define p (parameter-of n))
    (define type
      (or (option-value-type p)
          (zgal-error 'type-mismatch n "an option sets an int, uint, float or path parameter; ~a is ~a (flags set bools)"
                      (field-name p) (type-name (field-type p)))))
    (set-by (field-name p) n)
    (define default
      (and (eq? kind 'option)
           (let ([d (hash-ref opts '#:default (λ () (zgal-error 'bad-option c "an option needs a #:default: the parameter's value when it isn't given")))])
             (if (eq? type 'path)
                 (text d "a path's default")
                 (number-in d type "the default")))))
    (define range
      (match (hash-ref opts '#:range #f)
        [#f #f]
        [r (when (eq? type 'path) (zgal-error 'bad-option r "paths have no #:range"))
           (match (syntax->list r)
             [(list lo hi)
              (define range (cons (number-in lo type "the range's low end") (number-in hi type "the range's high end")))
              (unless (<= (car range) (cdr range)) (zgal-error 'bad-option r "the range's low end is above its high end"))
              (when (and default (not (<= (car range) default (cdr range))))
                (zgal-error 'bad-option r "the default, ~a, is outside the range" default))
              range]
             [_ (zgal-error 'malformed r "expected #:range (low high)")])]))
    (define given
      (match (hash-ref opts '#:given #f)
        [#f #f]
        [g (define gp (parameter-of g))
           (unless (equal? (field-type gp) (scalar-t 'bool))
             (zgal-error 'type-mismatch g "#:given sets a bool parameter; ~a is ~a" (field-name gp) (type-name (field-type gp))))
           (set-by (field-name gp) g)
           (field-name gp)]))
    (main-option kind p flag (text help "the option's help") default range given (stx->loc c)))

  (define options
    (for/list ([c (in-list clauses)]
               #:when (memq (head-symbol c) '(flag option positional)))
      (define kind (head-symbol c))
      (define-values (positional opts) (split-options (cdr (syntax->list c)) (format "~a" kind)))
      (match* (kind positional)
        [('flag (list n f help))
         (check-known-options opts '() "flag")
         (define p (parameter-of n))
         (unless (equal? (field-type p) (scalar-t 'bool))
           (zgal-error 'type-mismatch n "a flag sets a bool parameter; ~a is ~a" (field-name p) (type-name (field-type p))))
         (set-by (field-name p) n)
         (main-option 'flag p (claim-flag! f) (text help "the flag's help") #f #f #f (stx->loc c))]
        [('option (list n f help))
         (check-known-options opts '(#:default #:range #:given) "option")
         (option-of kind c n (claim-flag! f) help opts)]
        [('positional (list n help))
         (check-known-options opts '(#:range) "positional")
         (option-of kind c n #f help opts)]
        [('flag _) (zgal-error 'malformed c "expected (flag parameter \"--name\" \"help\")")]
        [('option _) (zgal-error 'malformed c "expected (option parameter \"--name\" \"help\" #:default value [#:range (low high)] [#:given flag-parameter])")]
        [('positional _) (zgal-error 'malformed c "expected (positional parameter \"help\" [#:range (low high)])")])))
  ;; The capture: an image readback of a graph the app runs
  (define capture
    (match (one 'capture)
      [#f #f]
      [c
       (define-values (positional opts) (split-options (cdr (syntax->list c)) "capture"))
       (check-known-options opts '(#:to) "capture")
       (define node-stx (match positional [(list n) n] [_ (zgal-error 'malformed c "expected (capture readback [#:to path-parameter])")]))
       (define node-name (format "~a" (syntax-e node-stx)))
       (define found
         (for*/list ([r (in-list (app-runs a))] [n (in-list (graph-nodes (graph-run-graph r)))] #:when (equal? (g-node-name n) node-name))
           (cons (graph-run-graph r) n)))
       (match-define (cons g n)
         (match found
           ['() (zgal-error 'unknown-name node-stx "no graph app ~a runs has a node ~a" (app-name a) node-name)]
           [(list f _ ...) f]))
       (unless (eq? (g-node-kind n) 'readback)
         (zgal-error 'type-mismatch node-stx "~a is a ~a node, not a readback" node-name (g-node-kind n)))
       (when (pair? (g-node-loops n))
         (zgal-error 'type-mismatch node-stx "readback ~a is in a loop, so it's a readback per iteration; main captures one" node-name))
       (unless (memq (entry-kind (hash-ref (graph-environment g) (g-node-subject n))) '(image import))
         (zgal-error 'type-mismatch node-stx "readback ~a reads a buffer; main captures an image" node-name))
       (define to
         (match (hash-ref opts '#:to #f)
           [#f #f]
           [t (define p (parameter-of t))
              (unless (equal? (option-value-type p) 'path)
                (zgal-error 'type-mismatch t "#:to names a path parameter; ~a is ~a" (field-name p) (type-name (field-type p))))
              (unless (for/or ([o (in-list options)]) (eq? (main-option-parameter o) p))
                (zgal-error 'bad-option t "#:to names a parameter an option sets, as the file is the user's choice"))
              (field-name p)]))
       (list (graph-name g) node-name to)]))
  (main-decl a about window timestep capture controls options (stx->loc stx)))
