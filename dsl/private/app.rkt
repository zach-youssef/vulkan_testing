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

(define event-kinds '(key-pressed key-released mouse-pressed mouse-released cursor-moved scrolled frame))

;; The C++ app class's values bodies can read besides their parameters (see cpp-app.rkt)
(define host-values
  (list (global 'host "framesInFlight" "context_->framesInFlight()" (scalar-t 'uint) #f)
        (global 'host "surfaceSize" "glm::vec2(float(graph_->surfaceExtent().width), float(graph_->surfaceExtent().height))"
                (vector-t 'float 2) #f)))

;; stx: the (app Name clause ...) form. structs, functions, constants: the file's, by name (hashes).
;; graphs: the file's graphs so far, by name.
(define (parse-app stx parse-type top-level structs functions constants graphs)
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
    (unless (memq (head-symbol c) '(parameter state on run))
      (zgal-error 'unknown-clause c "an app takes parameter, state, on and run clauses, not ~a" (show c))))
  (for ([key (in-list '(state run))])
    (when (> (length (clauses-of key)) 1)
      (zgal-error 'duplicate-clause (second (clauses-of key)) "app ~a has two (~a ...) clauses" name key)))

  ;; Parameters, given by C++
  (define parameters
    (for/list ([c (in-list (clauses-of 'parameter))])
      (match (syntax->list c)
        [(list _ n t)
         (define p-name (parse-name n "a parameter's name"))
         (names p-name n)
         (field p-name (parse-type t) (stx->loc c))]
        [_ (zgal-error 'malformed c "expected (parameter name type)")])))
  (define globals
    (for/fold ([h (for/hash ([g (in-list host-values)]) (values (global-name g) g))])
              ([p (in-list parameters)])
      (hash-set h (field-name p) (global 'host (field-name p) (format "inputs_.~a" (field-name p)) (field-type p) p))))
  ;; One context for the app's bodies, so their locals' spellings are unique in the class. They run
  ;; on the CPU, and can only call functions that compile to C++: the others take textures, samplers
  ;; or images, which apps don't have.
  (define ctx (make-context structs functions constants globals 'app))

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
                    (coerce (check v ctx (hash)) type v (format "field ~a's initial value" f-name)))]
           [_ (zgal-error 'malformed f "expected a state field (name type initial-value)")]))]))
  (define state-type (struct-t state))
  (define state-spelling (spell! ctx "state"))
  (define (state-locals where) (hash "state" (t-local state-spelling state-type (stx->loc where))))

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
         (define e (parse-event event-stx claim!))
         (define locals
           (if (event-binding e)
               (hash-set (state-locals c) (event-binding e)
                         (t-local (spell! ctx (event-binding e))
                                  (if (eq? (event-kind e) 'frame) (scalar-t 'float) (vector-t 'float 2))
                                  (stx->loc event-stx)))
               (state-locals c)))
         (define body (check-sequence body-stxs c ctx locals))
         (define next
           (if (void-t? (texpr-type body))
               ;; Only effects: the state doesn't change
               (t-begin (list body (hash-ref locals "state")) state-type (stx->loc c))
               (coerce body state-type c "a handler's result (the next state)")))
         (handler (struct-copy event e [binding (and (event-binding e) (t-local-name (hash-ref locals (event-binding e))))])
                  next (stx->loc c))]
        [_ (zgal-error 'malformed c "expected (on event body ...+)")])))

  ;; The graph it runs
  (define run
    (match (clauses-of 'run)
      ['() #f]
      [(list c) (parse-run c ctx state-locals graphs names)]))
  (app name parameters state initial handlers run (stx->loc stx)))

(define (parse-event stx claim!)
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
     (event kind keys #f #f (stx->loc stx))]
    [(or 'mouse-pressed 'mouse-released)
     (match args
       [(list b n)
        (define button (syntax-e b))
        (unless (memq button '(left right middle))
          (zgal-error 'malformed b "a mouse button is left, right or middle, not ~a" (show b)))
        (claim! (list kind button) stx)
        (event kind '() button (binding-name n) (stx->loc stx))]
       [_ (zgal-error 'malformed stx "expected (~a button cursor), binding the cursor's position" kind)])]
    [_
     (match args
       [(list n)
        (claim! (list kind #f) stx)
        (event kind '() #f (binding-name n) (stx->loc stx))]
       [_ (zgal-error 'malformed stx "expected (~a name), binding the event's value" kind)])]))

(define (parse-run stx ctx state-locals graphs names)
  (define parts (syntax->list stx))
  (unless (>= (length parts) 2)
    (zgal-error 'malformed stx "expected (run Graph (input expression) ... #:enabled ((node expression) ...))"))
  (define g-stx (second parts))
  (define g (or (and (symbol? (syntax-e g-stx)) (hash-ref graphs (symbol->string (syntax-e g-stx)) #f))
                (zgal-error 'unknown-name g-stx "~a isn't a graph declared earlier in the file" (show g-stx))))
  (define env (graph-environment g))
  (define-values (binding-stxs enabled-stx)
    (let loop ([rest (cddr parts)] [bindings '()] [enabled #f])
      (match rest
        ['() (values (reverse bindings) enabled)]
        [(list* k v more) #:when (eq? (syntax-e k) '#:enabled)
         (when enabled (zgal-error 'bad-option k "#:enabled is given twice"))
         (loop more bindings v)]
        [(cons k _) #:when (keyword? (syntax-e k))
         (zgal-error 'bad-option k "run takes no ~a option; it takes #:enabled" (syntax-e k))]
        [(cons b more) (loop more (cons b bindings) enabled)])))
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
             ;; Computed each frame, from the state too
             [(entry 'per-frame t _) (coerce (check v ctx (state-locals b)) t v (format "input ~a" input))]
             [#f (zgal-error 'unknown-name n "graph ~a has no parameter or input ~a" (graph-name g) input)]
             [(entry kind _ _)
              (zgal-error 'type-mismatch n "~a is ~a, which C++ gives (through the app's inputs); apps compute parameters and per-frame inputs"
                          input (case kind [(meshes) "a meshes input"] [(cpu) "a cpu input"] [(extern) "an extern input"]
                                  [(import import-buffer) "an imported resource"] [else "not an input"]))]))
         (cons input e)]
        [_ (zgal-error 'malformed b "expected (input expression)")])))
  ;; What the app doesn't compute, C++ gives: they're the app's inputs too
  (for ([item (in-list (graph-items g))]
        #:when (or (g-parameter? item) (g-input? item) (g-import? item) (g-import-buffer? item))
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
             (cons node-name (coerce (check v ctx (state-locals p)) (scalar-t 'bool) v (format "whether ~a is enabled" node-name)))]
            [_ (zgal-error 'malformed p "expected (node expression)")]))
        '()))
  (graph-run g bindings enables (stx->loc stx)))
