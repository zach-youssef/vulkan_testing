#lang racket/base
;; Reading and checking `app` forms: an application's parameters, state, the handlers computing its
;; next state from events, and the graph it runs with inputs computed from its state. Bodies are
;; checked like shader bodies (body.rkt), as the 'app stage, and run on the CPU.

(require racket/list
         racket/match
         racket/set
         racket/string
         "ast.rkt"
         "body.rkt"
         "errors.rkt"
         "graph.rkt"
         "layout.rkt"
         "syntax.rkt")

(provide parse-app
         key-names)

;; ZGAL's key names, and zgal::Key's enumerators (include/zgal/App.h)
(define key-names
  (append
   (for/list ([c (in-string "abcdefghijklmnopqrstuvwxyz")])
     (cons (string->symbol (string c)) (string (char-upcase c))))
   (for/list ([d (in-range 10)]) (cons d (format "Digit~a" d)))
   '((space . "Space") (escape . "Escape") (enter . "Enter") (tab . "Tab") (backspace . "Backspace")
     (left . "Left") (right . "Right") (up . "Up") (down . "Down") (minus . "Minus") (equal . "Equal")
     (keypad-add . "KeypadAdd") (keypad-subtract . "KeypadSubtract"))
   (for/list ([n (in-range 1 13)]) (cons (string->symbol (format "f~a" n)) (format "F~a" n)))))

(define event-kinds '(key-pressed key-released mouse-pressed mouse-released cursor-moved scrolled frame read))

;; The C++ app class's values bodies can read besides their parameters (see cpp-app.rkt)
(define host-values
  (list (global 'host "framesInFlight" "context_->framesInFlight()" (scalar-t 'uint) #f)
        (global 'host "surfaceSize" "glm::vec2(float(active_->surfaceExtent().width), float(active_->surfaceExtent().height))"
                (vector-t 'float 2) #f)
        (global 'host "frameNumber" "frameNumber_" (scalar-t 'uint) #f)
        (global 'host "headless" "inputs_.mode.headless()" (scalar-t 'bool) #f)
        (global 'host "capture" "inputs_.mode.capture()" (scalar-t 'bool) #f)
        (global 'host "frameLimit" "inputs_.mode.frameLimit()" (scalar-t 'uint) #f)))

;; The host values that come from how the app is run (zgal::RunMode, in its Inputs)
(define run-mode-values '("headless" "capture" "frameLimit"))

;; stx: the (app Name clause ...) form. structs, functions, constants: the file's, by name (hashes).
;; cpu-functions: the names of those compiled to C++ (a set). graphs: the file's graphs so far, by
;; name.
(define (parse-app stx parse-file-type top-level structs functions constants cpu-functions graphs)
  ;; Apps' values can also be data: (data T)
  (define (parse-type stx)
    (match (syntax->list stx)
      [(list d t) #:when (eq? (syntax-e d) 'data)
       (define element (parse-file-type t))
       (when (or (array-t? element) (std430-problem (array-t element 1)))
         (zgal-error 'type-not-allowed t "data holds elements laid out like a buffer's: ~a" (or (std430-problem (array-t element 1)) "not arrays")))
       (data-t element)]
      [_ (parse-file-type stx)]))
  (define items (syntax->list stx))
  (unless (and items (>= (length items) 2))
    (zgal-error 'malformed stx "expected (app Name clause ...)"))
  (define name (parse-name (second items) "the app's name"))
  (top-level name (second items))
  (define names (make-namespace (format "in app ~a" name)))
  (for ([h (in-list host-values)]) (names (global-name h) (second items)))

  (define clauses (cddr items))
  (define (clauses-of key) (filter (λ (c) (eq? (head-symbol c) key)) clauses))
  (for ([c (in-list clauses)])
    (unless (memq (head-symbol c) '(parameter extern state on run))
      (zgal-error 'unknown-clause c "an app takes parameter, extern, state, on and run clauses, not ~a" (show c))))
  (for ([key (in-list '(state))])
    (when (> (length (clauses-of key)) 1)
      (zgal-error 'duplicate-clause (second (clauses-of key)) "app ~a has two (~a ...) clauses" name key)))

  ;; Parameters, given by C++. They may also be paths, which graphs' path inputs take.
  (define parameters
    (for/list ([c (in-list (clauses-of 'parameter))])
      (match (syntax->list c)
        [(list _ n t)
         (define p-name (parse-name n "a parameter's name"))
         (names p-name n)
         (field p-name (if (eq? (syntax-e t) 'path) (opaque-t 'path) (parse-type t)) (stx->loc c))]
        [_ (zgal-error 'malformed c "expected (parameter name type)")])))
  (define globals
    (for/fold ([h (for/hash ([g (in-list host-values)]) (values (global-name g) g))])
              ([p (in-list parameters)])
      (hash-set h (field-name p) (global 'host (field-name p) (format "inputs_.~a" (field-name p)) (field-type p) p))))
  ;; Functions C++ gives, which bodies call like the file's
  (define externs
    (for/list ([c (in-list (clauses-of 'extern))])
      (match (syntax->list c)
        [(list _ signature colon result) #:when (and (syntax->list signature) (eq? (syntax-e colon) ':))
         (match-define (cons n params) (syntax->list signature))
         (define f-name (parse-called-name n "an extern function's name"))
         (names f-name n)
         (when (hash-ref functions f-name #f)
           (zgal-error 'duplicate-name n "~a is already a function of the file" f-name))
         (define param-names (make-namespace (format "as a parameter of ~a" f-name)))
         (function-decl f-name
                        (for/list ([p (in-list params)])
                          (match (syntax->list p)
                            [(list pn colon2 t) #:when (eq? (syntax-e colon2) ':)
                             (define p-name (parse-name pn "a parameter's name"))
                             (param-names p-name pn)
                             (cons p-name (parse-type t))]
                            [_ (zgal-error 'malformed p "expected a parameter [name : type]")]))
                        (if (eq? (syntax-e result) 'void) (void-t) (parse-type result))
                        #f #t '() (stx->loc c))]
        [_ (zgal-error 'malformed c "expected (extern (name [parameter : type] ...) : result-type)")])))
  (define app-functions
    (let ([h (hash-copy functions)])
      (for ([f (in-list externs)]) (hash-set! h (function-decl-name f) f))
      h))

  ;; One context for the app's bodies, so their locals' spellings are unique in the class. They run
  ;; on the CPU, so they only call functions that compile to C++ (and their externs).
  (define ctx (make-context structs app-functions constants globals 'app))
  (define cpu-callable (for/fold ([s (list->set (set->list cpu-functions))]) ([f (in-list externs)]) (set-add s (function-decl-name f))))
  (define (cpu-only e) (check-cpu-only e cpu-callable) e)

  ;; The state: fields and their initial values, computed from the parameters
  (define-values (state initial)
    (match (clauses-of 'state)
      ['() (values (struct-decl "State" '() (stx->loc stx)) '())]
      [(list c)
       (define field-names (make-namespace "in the app's state"))
       (for/lists (fields initial #:result (values (struct-decl "State" fields (stx->loc c)) initial))
                  ([f (in-list (cdr (syntax->list c)))])
         (match (syntax->list f)
           [(list n t v)
            (define f-name (parse-name n "a state field's name"))
            (field-names f-name n)
            (define type (parse-type t))
            (values (field f-name type (stx->loc f))
                    (cpu-only (coerce (check v ctx (hash)) type v (format "field ~a's initial value" f-name))))]
           [_ (zgal-error 'malformed f "expected a state field (name type initial-value)")]))]))
  (define state-type (struct-t state))
  (define state-spelling (spell! ctx "state"))
  (define (state-locals where) (hash "state" (t-local state-spelling state-type (stx->loc where))))

  ;; A readback node of a graph the app runs, for read events: (list graph node element-type)
  (define (find-readback node-stx)
    (define node-name (format "~a" (syntax-e node-stx)))
    (define found
      (for*/list ([c (in-list (clauses-of 'run))]
                  [g (in-value (let ([g-stx (and (pair? (cdr (syntax->list c))) (second (syntax->list c)))])
                                 (and g-stx (symbol? (syntax-e g-stx)) (hash-ref graphs (symbol->string (syntax-e g-stx)) #f))))]
                  #:when g
                  [n (in-list (graph-nodes g))]
                  #:when (equal? (g-node-name n) node-name))
        (cons g n)))
    (match found
      ['() (zgal-error 'unknown-name node-stx "no graph app ~a runs has a node ~a" name node-name)]
      [(list (cons g n) _ ...)
       (unless (eq? (g-node-kind n) 'readback)
         (zgal-error 'type-mismatch node-stx "~a is a ~a node, not a readback" node-name (g-node-kind n)))
       (when (pair? (g-node-loops n))
         (zgal-error 'type-mismatch node-stx "readback ~a is in a loop, so it's a readback per iteration; read events take one readback" node-name))
       (define subject (hash-ref (graph-environment g) (g-node-subject n)))
       (unless (memq (entry-kind subject) '(buffer import-buffer upload))
         (zgal-error 'type-mismatch node-stx "readback ~a reads an image; read events give buffers' elements" node-name))
       (list (graph-name g) node-name (entry-type subject))]))

  ;; Handlers
  (define handled (make-hash))   ; (list kind key-or-button) -> loc
  (define (claim! key where)
    (match (hash-ref handled key #f)
      [#f (hash-set! handled key (stx->loc where))]
      [previous (zgal-error 'duplicate-name where "~a is already handled (line ~a)"
                            (string-join (map (λ (x) (format "~a" x)) (filter values key)) " ") (loc-line previous))]))
  (define handlers
    (for/list ([c (in-list (clauses-of 'on))])
      (match (syntax->list c)
        [(list* _ event-stx body-stxs) #:when (pair? body-stxs)
         (define e (parse-event event-stx claim! (λ (node-stx) (find-readback node-stx))))
         (define locals
           (if (event-binding e)
               (hash-set (state-locals c) (event-binding e)
                         (t-local (spell! ctx (event-binding e))
                                  (case (event-kind e)
                                    [(frame) (scalar-t 'float)]
                                    [(read) (data-t (third (event-readback e)))]
                                    [else (vector-t 'float 2)])
                                  (stx->loc event-stx)))
               (state-locals c)))
         (define body (check-sequence body-stxs c ctx locals))
         (define next
           (if (void-t? (texpr-type body))
               ;; Only effects: the state doesn't change
               (t-begin (list body (hash-ref locals "state")) state-type (stx->loc c))
               (coerce body state-type c "a handler's result (the next state)")))
         (handler (struct-copy event e [binding (and (event-binding e) (t-local-name (hash-ref locals (event-binding e))))])
                  (cpu-only next) (stx->loc c))]
        [_ (zgal-error 'malformed c "expected (on event body ...+)")])))

  ;; The graphs it runs: the active one is the first whose condition holds, so only the last has
  ;; none (and an app running one graph always runs it)
  (define runs
    (for/list ([c (in-list (clauses-of 'run))])
      (parse-run c ctx state-locals graphs names cpu-only)))
  (for ([r (in-list runs)] [i (in-naturals)])
    (define last? (= i (sub1 (length runs))))
    (cond
      [(and last? (graph-run-when r))
       (zgal-error 'bad-option (graph-run-loc r) "the last graph an app runs has no #:when: it runs when no other's condition holds")]
      [(and (not last?) (not (graph-run-when r)))
       (zgal-error 'bad-option (graph-run-loc r) "an app running several graphs gives every graph but the last a #:when condition, so one is chosen each frame")]))
  (let ([seen (make-hash)])
    (for ([r (in-list runs)])
      (define g-name (graph-name (graph-run-graph r)))
      (when (hash-ref seen g-name #f)
        (zgal-error 'duplicate-name (graph-run-loc r) "app ~a runs graph ~a twice" name g-name))
      (hash-set! seen g-name #t)))
  ;; Whether any body reads how the app is run, which C++ then gives
  (define run-mode?
    (for*/or ([e (in-list (append initial (map handler-body handlers)
                                  (append-map (λ (r) (append (map cdr (graph-run-bindings r)) (map cdr (graph-run-enables r))
                                                             (if (graph-run-when r) (list (graph-run-when r)) '())))
                                              runs)))]
              [x (in-list (subexpressions e))])
      (match x
        [(t-global 'host n _ _ _) (and (member n run-mode-values) #t)]
        [_ #f])))
  (app name parameters externs state initial handlers runs run-mode? (stx->loc stx)))

(define (parse-event stx claim! find-readback)
  (define parts (syntax->list stx))
  (define kind (head-symbol stx))
  (unless (memq kind event-kinds)
    (zgal-error 'malformed stx "expected an event: ~a" (string-join (map symbol->string event-kinds) ", ")))
  (define args (cdr parts))
  (define (binding-name n) (parse-name n "the name the event's value is bound to"))
  (match kind
    [(or 'key-pressed 'key-released)
     (when (null? args)
       (zgal-error 'malformed stx "expected (~a key ...+)" kind))
     (define keys
       (for/list ([k (in-list args)])
         (define key (syntax-e k))
         (unless (assoc key key-names)
           (zgal-error 'malformed k "~a isn't a key; keys are a-z, 0-9, ~a" (show k)
                       (string-join (for/list ([p (in-list key-names)] #:when (and (symbol? (car p)) (> (string-length (symbol->string (car p))) 1)))
                                      (symbol->string (car p)))
                                    ", ")))
         (claim! (list kind key) k)
         key))
     (event kind keys #f #f #f (stx->loc stx))]
    [(or 'mouse-pressed 'mouse-released)
     (match args
       [(list b n)
        (define button (syntax-e b))
        (unless (memq button '(left right middle))
          (zgal-error 'malformed b "a mouse button is left, right or middle, not ~a" (show b)))
        (claim! (list kind button) stx)
        (event kind '() button (binding-name n) #f (stx->loc stx))]
       [_ (zgal-error 'malformed stx "expected (~a button cursor), binding the cursor's position" kind)])]
    ['read
     (match args
       [(list node n)
        (define readback (find-readback node))
        (claim! (list kind (second readback)) stx)
        (event kind '() #f (binding-name n) readback (stx->loc stx))]
       [_ (zgal-error 'malformed stx "expected (read readback values), binding the elements the readback copied")])]
    [_
     (match args
       [(list n)
        (claim! (list kind #f) stx)
        (event kind '() #f (binding-name n) #f (stx->loc stx))]
       [_ (zgal-error 'malformed stx "expected (~a name), binding the event's value" kind)])]))

(define (parse-run stx ctx state-locals graphs names cpu-only)
  (define parts (syntax->list stx))
  (unless (>= (length parts) 2)
    (zgal-error 'malformed stx "expected (run Graph (input expression) ... #:enabled ((node expression) ...) #:when condition)"))
  (define g-stx (second parts))
  (define g (or (and (symbol? (syntax-e g-stx)) (hash-ref graphs (symbol->string (syntax-e g-stx)) #f))
                (zgal-error 'unknown-name g-stx "~a isn't a graph declared earlier in the file" (show g-stx))))
  (define env (graph-environment g))
  (define-values (binding-stxs options)
    (let loop ([rest (cddr parts)] [bindings '()] [options (hasheq)])
      (match rest
        ['() (values (reverse bindings) options)]
        [(list* k v more) #:when (memq (syntax-e k) '(#:enabled #:when))
         (when (hash-ref options (syntax-e k) #f) (zgal-error 'bad-option k "~a is given twice" (syntax-e k)))
         (loop more bindings (hash-set options (syntax-e k) v))]
        [(cons k _) #:when (keyword? (syntax-e k))
         (zgal-error 'bad-option k "run takes no ~a option; it takes #:enabled and #:when" (syntax-e k))]
        [(cons b more) (loop more (cons b bindings) options)])))
  (define enabled-stx (hash-ref options '#:enabled #f))
  ;; Whether this is the active graph, computed each frame from the state
  (define active
    (let ([c (hash-ref options '#:when #f)])
      (and c (cpu-only (coerce (check c ctx (state-locals c)) (scalar-t 'bool) c "whether the graph runs")))))
  (define bound (make-hash))
  (define bindings
    (for/list ([b (in-list binding-stxs)])
      (match (syntax->list b)
        [(list n v)
         (define input (format "~a" (syntax-e n)))
         (when (hash-ref bound input #f)
           (zgal-error 'duplicate-name n "~a is given twice" input))
         (hash-set! bound input #t)
         (define e
           (match (hash-ref env input #f)
             ;; Computed once, from the app's parameters
             [(entry 'parameter t _) (coerce (check v ctx (hash)) t v (format "parameter ~a" input))]
             [(entry 'path _ _) (coerce (check v ctx (hash)) (opaque-t 'path) v (format "path ~a" input))]
             ;; Computed each frame, from the state too
             [(entry 'per-frame t _) (coerce (check v ctx (state-locals b)) t v (format "input ~a" input))]
             [(entry 'per-frame-data t _) (coerce (check v ctx (state-locals b)) (data-t t) v (format "input ~a" input))]
             [#f (zgal-error 'unknown-name n "graph ~a has no parameter or input ~a" (graph-name g) input)]
             [(entry kind _ _)
              (zgal-error 'type-mismatch n "~a is ~a, which C++ gives (through the app's inputs); apps compute parameters and per-frame inputs"
                          input (case kind [(meshes) "a meshes input"] [(cpu) "a cpu input"] [(extern) "an extern input"]
                                  [(import import-images import-buffer) "an imported resource"] [(data) "a data input"] [else "not an input"]))]))
         (cons input (cpu-only e))]
        [_ (zgal-error 'malformed b "expected (input expression)")])))
  ;; What the app doesn't compute, C++ gives: they're the app's inputs too
  (for ([item (in-list (graph-items g))]
        #:when (or (g-parameter? item) (g-input? item) (g-import? item) (g-import-images? item) (g-import-buffer? item))
        #:unless (hash-ref bound (graph-item-name item) #f))
    (names (graph-item-name item) stx))
  (define enables
    (if enabled-stx
        (for/list ([p (in-list (or (syntax->list enabled-stx) (zgal-error 'malformed enabled-stx "#:enabled takes ((node expression) ...)")))])
          (match (syntax->list p)
            [(list n v)
             (define node-name (format "~a" (syntax-e n)))
             (define node (or (for/first ([nd (in-list (graph-nodes g))] #:when (equal? (g-node-name nd) node-name)) nd)
                              (zgal-error 'unknown-name n "graph ~a has no node ~a" (graph-name g) node-name)))
             (when (eq? (g-node-kind node) 'extern)
               (zgal-error 'type-mismatch n "~a is an extern node, whose nodes C++ adds and enables" node-name))
             (when (g-node-once? node)
               (zgal-error 'type-mismatch n "~a runs once (#:once), so the app doesn't enable it" node-name))
             (cons node-name (cpu-only (coerce (check v ctx (state-locals p)) (scalar-t 'bool) v (format "whether ~a is enabled" node-name))))]
            [_ (zgal-error 'malformed p "expected (node expression)")]))
        '()))
  (graph-run g bindings enables active (stx->loc stx)))
