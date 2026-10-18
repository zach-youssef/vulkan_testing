#lang racket/base
;; Shader bodies written in ZGAL: reading and type-checking them into typed expressions, and
;; inferring which bindings they read and write. The typed expressions are backend independent;
;; lower.rkt turns them into statements for the GLSL and C++ printers.

(require racket/list
         racket/match
         racket/set
         racket/string
         "ast.rkt"
         "builtins.rkt"
         "errors.rkt"
         "layout.rkt"
         "syntax.rkt")

(provide (all-defined-out))

;; ---------------------------------------------------------------------------------------------
;; Typed expressions. Every node has a type and a loc (where it is in the .zgal file).

(struct t-lit (value type loc) #:transparent)                 ; a number or boolean
(struct t-local (name type loc) #:transparent)                ; name: as the backends spell it
;; A name declared outside the body. kind: 'binding 'push 'varying 'input 'stage 'constant 'shared,
;; or 'host: a value an app's C++ computes (its parameters, framesInFlight, surfaceSize).
;; spelling: the GLSL spelling for 'input and 'stage, the C++ expression for 'host
(struct t-global (kind name spelling type loc) #:transparent)
(struct t-call (function args type loc) #:transparent)        ; a built-in function (a symbol)
(struct t-user-call (name args type loc) #:transparent)       ; a function of the file
(struct t-texture (op args type loc) #:transparent)           ; 'sample 'sample-lod 'fetch 'sample-compare 'texture-size 'image-load 'image-size
(struct t-op (op args type loc) #:transparent)                ; an operator, e.g. '+ 'bitand '<
(struct t-construct (args type loc) #:transparent)            ; a vector, matrix, scalar, struct or array
(struct t-field (base name type loc) #:transparent)
(struct t-swizzle (base components type loc) #:transparent)   ; components: "xyz" (always xyzw letters)
(struct t-index (base index type loc) #:transparent)
(struct t-if (condition then else type loc) #:transparent)   ; else is #f for when
(struct t-let (bindings body type loc) #:transparent)         ; bindings: (list names types expression)
(struct t-fold (accumulators loops body type loc) #:transparent) ; accumulators: (list name type init); loops: (list name from to step)
(struct t-for (loops body loc) #:transparent)                 ; a loop for effects
(struct t-while (accumulators condition body type loc) #:transparent) ; accumulators as t-fold's
(struct t-values (expressions type loc) #:transparent)
(struct t-begin (expressions type loc) #:transparent)
(struct t-store (image texel value loc) #:transparent)        ; imageStore
(struct t-write (place value loc) #:transparent)              ; place: a global binding, field or index of one
(struct t-results (fields loc) #:transparent)                 ; (cons output-name expression)
(struct t-with (base fields type loc) #:transparent)          ; fields: (cons name expression)
(struct t-barrier (buffers? loc) #:transparent)               ; a compute shader's workgroup barrier (also ordering buffer writes)
(struct t-effect (op args loc) #:transparent)                 ; an app's effect: 'print 'set-frames-in-flight 'resize 'quit
;; An atomic operation on a place (as t-write's), giving its previous value. op: 'add 'min 'max 'and
;; 'or 'xor 'exchange 'compare-exchange (whose args are the comparand, then the value).
(struct t-atomic (op place args type loc) #:transparent)
(struct t-exit (op loc) #:transparent)                        ; 'return (compute) or 'discard (fragment)

;; The type of a stage's results
(struct results-t () #:transparent)

(define (texpr-type e)
  (match e
    [(t-lit _ t _) t] [(t-local _ t _) t] [(t-global _ _ _ t _) t] [(t-call _ _ t _) t]
    [(t-user-call _ _ t _) t] [(t-texture _ _ t _) t] [(t-op _ _ t _) t] [(t-construct _ t _) t]
    [(t-field _ _ t _) t] [(t-swizzle _ _ t _) t] [(t-index _ _ t _) t] [(t-if _ _ _ t _) t]
    [(t-let _ _ t _) t] [(t-fold _ _ _ t _) t] [(t-while _ _ _ t _) t] [(t-for _ _ _) (void-t)] [(t-values _ t _) t]
    [(t-begin _ t _) t] [(t-store _ _ _ _) (void-t)] [(t-write _ _ _) (void-t)]
    [(t-results _ _) (results-t)] [(t-with _ _ t _) t] [(t-barrier _ _) (void-t)] [(t-effect _ _ _) (void-t)]
    [(t-atomic _ _ _ t _) t] [(t-exit _ _) (void-t)]))

(define (texpr-loc e)
  (match e
    [(t-lit _ _ l) l] [(t-local _ _ l) l] [(t-global _ _ _ _ l) l] [(t-call _ _ _ l) l]
    [(t-user-call _ _ _ l) l] [(t-texture _ _ _ l) l] [(t-op _ _ _ l) l] [(t-construct _ _ l) l]
    [(t-field _ _ _ l) l] [(t-swizzle _ _ _ l) l] [(t-index _ _ _ l) l] [(t-if _ _ _ _ l) l]
    [(t-let _ _ _ l) l] [(t-fold _ _ _ _ l) l] [(t-while _ _ _ _ l) l] [(t-for _ _ l) l] [(t-values _ _ l) l]
    [(t-begin _ _ l) l] [(t-store _ _ _ l) l] [(t-write _ _ l) l] [(t-results _ l) l] [(t-with _ _ _ l) l]
    [(t-barrier _ l) l] [(t-effect _ _ l) l] [(t-atomic _ _ _ _ l) l] [(t-exit _ l) l]))

;; Applies f to e's subexpressions, returning a list of the results
(define (texpr-children e)
  (match e
    [(or (t-lit _ _ _) (t-local _ _ _) (t-global _ _ _ _ _) (t-barrier _ _) (t-exit _ _)) '()]
    [(t-atomic _ place args _ _) (cons place args)]
    [(t-call _ args _ _) args] [(t-user-call _ args _ _) args] [(t-texture _ args _ _) args]
    [(t-op _ args _ _) args] [(t-construct args _ _) args]
    [(t-field b _ _ _) (list b)] [(t-swizzle b _ _ _) (list b)] [(t-index b i _ _) (list b i)]
    [(t-if c a b _ _) (filter values (list c a b))]
    [(t-let bindings body _ _) (append (map third bindings) (list body))]
    [(t-fold accs loops body _ _) (append (map third accs) (append-map cdr loops) (list body))]
    [(t-for loops body _) (append (append-map cdr loops) (list body))]
    [(t-while accs c body _ _) (append (map third accs) (list c body))]
    [(t-values es _ _) es] [(t-begin es _ _) es]
    [(t-store i t v _) (list i t v)] [(t-write p v _) (list p v)]
    [(t-results fields _) (map cdr fields)] [(t-with b fields _ _) (cons b (map cdr fields))]
    [(t-effect _ args _) args]))

;; Everything in e, including itself
(define (subexpressions e)
  (cons e (append-map subexpressions (texpr-children e))))

(define (describe t)
  (cond [(literal? t) "integer literal"]
        [(results-t? t) "stage results"]
        [else (type-name t)]))
(define (a-type t)
  (define d (describe t))
  (string-append (if (memv (string-ref d 0) '(#\a #\e #\i #\o)) "an " "a ") d))

;; ---------------------------------------------------------------------------------------------
;; Literal types

;; e with its integer literals given type t (e has the literal type)
(define (retype e t)
  (match e
    [(t-lit v _ l) (t-lit (if (eq? (scalar-t-kind t) 'float) (exact->inexact v) v) t l)]
    [(t-op op args _ l) (t-op op (for/list ([a (in-list args)]) (if (literal? (texpr-type a)) (retype a t) a)) t l)]
    [(t-if c a b _ l) (t-if c (retype a t) (retype b t) t l)]
    ;; A constant of integer literals (see literal-constant?): declared as an int, converted where
    ;; it's used as anything else
    [(t-global 'constant name spelling _ l)
     (define declared (t-global 'constant name spelling (scalar-t 'int) l))
     (if (equal? t (scalar-t 'int)) declared (t-construct (list declared) t l))]))

;; e as type t: integer literals take it, and anything else must have it
(define (coerce e t where what)
  (define et (texpr-type e))
  (cond
    [(equal? et t) e]
    [(and (literal? et) (scalar-t? t) (numeric-kind? (scalar-t-kind t))) (retype e t)]
    [(and (literal? et) (scalar-t? t) (small-integer-kind? (scalar-t-kind t)))
     (small-literal e t where)]
    [else (zgal-error 'type-mismatch where "~a is ~a, not ~a" what (a-type t) (a-type et))]))

;; An integer literal as small integer type t: a literal itself (an expression of literals must be
;; converted explicitly), in t's range
(define (small-literal e t where)
  (match e
    [(t-lit v _ l)
     (define bits (small-integer-bits (scalar-t-kind t)))
     (define-values (low high)
       (if (memq (scalar-t-kind t) '(uint8 uint16))
           (values 0 (sub1 (expt 2 bits)))
           (values (- (expt 2 (sub1 bits))) (sub1 (expt 2 (sub1 bits))))))
     (unless (<= low v high)
       (zgal-error 'type-mismatch where "~a doesn't fit in a ~a (~a to ~a)" v (type-name t) low high))
     (t-lit v t l)]
    [_ (zgal-error 'type-mismatch where "an expression of integer literals isn't a ~a: convert it with (~a expression)"
                   (type-name t) (type-name t))]))

;; Small integers are storage types: reading one gives its 32-bit kind (uint or int), so arithmetic
;; and built-ins never see them. Only narrowing to one, (uint8 x), keeps the small type, for
;; writing it to a buffer, shared variable or struct field.
(define (widen e)
  (match (texpr-type e)
    [(scalar-t k) #:when (small-integer-kind? k)
     (t-construct (list e) (scalar-t (widened-kind k)) (texpr-loc e))]
    [_ e]))

;; Whether constant c's value is an expression of integer literals (and such constants), which
;; makes it one too wherever it's used: (define tiles 57) compares with uints as 57 does. It's
;; declared as an int, the type literals settle on. An explicit (int 57) isn't one.
(define (literal-constant? c constants)
  (let shaped? ([e (constant-decl-body c)])
    (match e
      [(t-lit v (scalar-t 'int) _) (exact-integer? v)]
      [(t-op op args (scalar-t 'int) _)
       (and (memq op '(+ - * / remainder bitand bitor bitxor shl shr negate bitnot)) (andmap shaped? args))]
      [(t-global 'constant name _ (scalar-t 'int) _)
       (define d (hash-ref constants name #f))
       (and d (literal-constant? d constants))]
      [_ #f])))

;; Integer literals left alone are ints
(define (settle e) (if (literal? (texpr-type e)) (retype e (scalar-t 'int)) e))

;; ---------------------------------------------------------------------------------------------
;; Scope

;; What a body can name besides its locals. kind: 'binding 'push 'varying 'input 'stage 'constant,
;; or 'output: a stage's outputs, which locals can't hide (they'd be written instead) and bodies
;; can't read
(struct global (kind name spelling type item) #:transparent)

;; The checking context: the file's structs, functions and constants (by name), the names the
;; body sees (globals), its stage (or 'function), and the mutable state of one function or shader:
;; the spellings used for locals, and the functions and constants used
(struct context (structs functions constants globals stage spellings uses) #:transparent)

(define (make-context structs functions constants globals stage)
  (context structs functions constants globals stage (make-hash) (mutable-set)))

;; A fresh spelling for local `name`: itself the first time in a function, then name_2, ...
(define (spell! ctx name)
  (define n (add1 (hash-ref (context-spellings ctx) name 0)))
  (hash-set! (context-spellings ctx) name n)
  (if (= n 1) name (format "~a_~a" name n)))

;; ---------------------------------------------------------------------------------------------
;; Checking

(define swizzle-sets '("xyzw" "rgba" "stpq"))

(define (swizzle-components s size)
  (and (<= 1 (string-length s) 4)
       (for/or ([set (in-list swizzle-sets)])
         (and (for/and ([c (in-string s)])
                (define i (for/first ([d (in-string set)] [k (in-naturals)] #:when (char=? c d)) k))
                (and i (< i size)))
              (list->string (for/list ([c (in-string s)])
                              (string-ref "xyzw" (for/first ([d (in-string set)] [k (in-naturals)] #:when (char=? c d)) k))))))))

;; Field or swizzle `name` of typed e
(define (member-of e name where)
  (match (texpr-type e)
    [(struct-t d)
     (define f (or (for/first ([f (in-list (struct-decl-fields d))] #:when (string=? (field-name f) name)) f)
                   (zgal-error 'unknown-name where "struct ~a has no field ~a" (struct-decl-name d) name)))
     (t-field e name (field-type f) (stx->loc where))]
    [(vector-t k n)
     (define comps (or (swizzle-components name n)
                       (zgal-error 'unknown-name where "~a isn't a swizzle of ~a (use up to four of xyzw, rgba or stpq)"
                                   name (type-name (vector-t k n)))))
     (t-swizzle e comps (if (= (string-length comps) 1) (scalar-t k) (vector-t k (string-length comps))) (stx->loc where))]
    [t (zgal-error 'type-mismatch where "~a has no fields: it's ~a" name (a-type t))]))

(define vector-constructors
  (for*/hash ([kind '(float int uint bool)] [n '(2 3 4)])
    (define t (vector-t kind n))
    (values (string->symbol (type-name t)) t)))
(define matrix-constructors (hash 'mat2 (matrix-t 2) 'mat3 (matrix-t 3) 'mat4 (matrix-t 4)))
(define scalar-constructors
  (for/fold ([h (hash 'float (scalar-t 'float) 'int (scalar-t 'int) 'uint (scalar-t 'uint) 'bool (scalar-t 'bool))])
            ([k (in-list small-integer-kinds)])
    (hash-set h k (scalar-t k))))

(define (components t)
  (match t
    [(scalar-t _) 1]
    [(vector-t _ n) n]
    [(matrix-t n) (* n n)]
    [_ #f]))

(define (numeric-type? t)
  (or (literal? t)
      (and (scalar-t? t) (numeric-kind? (scalar-t-kind t)))
      (and (vector-t? t) (numeric-kind? (vector-t-kind t)))
      (matrix-t? t)))
(define (element-kind t) (if (matrix-t? t) 'float (component-kind t)))

;; The type of arithmetic op on a and b (typed), as (list a b result)
(define (arithmetic op a b where)
  (define ta (texpr-type a))
  (define tb (texpr-type b))
  (unless (and (numeric-type? ta) (numeric-type? tb))
    (zgal-error 'type-mismatch where "~a takes numbers, vectors and matrices, not ~a and ~a" op (a-type ta) (a-type tb)))
  (cond
    [(and (literal? ta) (literal? tb)) (list a b literal-type)]
    [(literal? ta) (list (retype a (scalar-t (element-kind tb))) b tb)]
    [(literal? tb) (list a (retype b (scalar-t (element-kind ta))) ta)]
    [(equal? ta tb) (list a b ta)]
    [(and (scalar-t? ta) (equal? (scalar-t-kind ta) (element-kind tb))) (list a b tb)]
    [(and (scalar-t? tb) (equal? (scalar-t-kind tb) (element-kind ta))) (list a b ta)]
    [(and (eq? op '*) (matrix-t? ta) (vector-t? tb) (eq? (vector-t-kind tb) 'float) (= (matrix-t-size ta) (vector-t-size tb)))
     (list a b tb)]
    [(and (eq? op '*) (vector-t? ta) (matrix-t? tb) (eq? (vector-t-kind ta) 'float) (= (matrix-t-size tb) (vector-t-size ta)))
     (list a b ta)]
    [else (zgal-error 'type-mismatch where "~a of ~a and ~a isn't defined" op (a-type ta) (a-type tb))]))

;; Integer operators: same int or uint scalars or vectors
(define (integer-op op a b where)
  (define ta (texpr-type a))
  (define tb (texpr-type b))
  (define (integral? t) (or (literal? t) (memq (component-kind t) '(int uint))))
  (unless (and (integral? ta) (integral? tb))
    (zgal-error 'type-mismatch where "~a takes ints and uints, not ~a and ~a" op (a-type ta) (a-type tb)))
  (cond
    [(and (literal? ta) (literal? tb)) (list a b literal-type)]
    [(literal? ta) (list (retype a (scalar-t (component-kind tb))) b tb)]
    [(literal? tb) (list a (retype b (scalar-t (component-kind ta))) ta)]
    [(equal? ta tb) (list a b ta)]
    [(and (memq op '(shl shr)) (scalar-t? tb)) (list a b ta)]
    [else (zgal-error 'type-mismatch where "~a of ~a and ~a isn't defined" op (a-type ta) (a-type tb))]))

(define (zero t where)
  (match t
    [(scalar-t k) (t-lit (if (eq? k 'float) 0.0 0) t where)]
    [(vector-t k _) (t-construct (list (t-lit (if (eq? k 'float) 0.0 0) (scalar-t k) where)) t where)]
    [(matrix-t _) (t-construct (list (t-lit 0.0 (scalar-t 'float) where)) t where)]
    [_ (zgal-error 'type-mismatch where "sum adds numbers, vectors and matrices, not ~a" (a-type t))]))

;; Checks expression stx in ctx, with locals (a hash from names to t-locals). Small integers read
;; are widened (see widen), unless the expression narrows to one.
(define (check stx ctx locals)
  (define e (check-raw stx ctx locals))
  (if (memq (head-symbol stx) small-integer-kinds) e (widen e)))

;; check, without widening the value: for places written to
(define (check-raw stx ctx locals)
  (define (recur s [ls locals]) (check s ctx ls))
  (define here (stx->loc stx))
  (define d (syntax-e stx))
  (cond
    [(boolean? d) (t-lit d (scalar-t 'bool) here)]
    [(exact-integer? d) (t-lit d literal-type here)]
    [(real? d) (t-lit (exact->inexact d) (scalar-t 'float) here)]
    [(symbol? d) (check-name (symbol->string d) stx ctx locals)]
    [(and (syntax->list stx) (pair? (syntax->list stx)) (symbol? (syntax-e (car (syntax->list stx)))))
     (check-form stx ctx locals)]
    [else (zgal-error 'malformed stx "expected an expression, found ~a" (show stx))]))

;; A name: a local, a global, a constant or a stage variable, followed by .field or .swizzle parts
(define (check-name full stx ctx locals)
  (define parts (string-split full "." #:trim? #f))
  (when (or (null? parts) (ormap (λ (p) (string=? p "")) parts))
    (zgal-error 'malformed stx "~a isn't a name: write fields and swizzles as name.field" full))
  (define base-name (first parts))
  (define here (stx->loc stx))
  (define base
    (cond
      [(hash-ref locals base-name #f)
       => (λ (l) (if (t-local? l)
                     (t-local (t-local-name l) (t-local-type l) here)
                     ;; An alias of a resource (see check-let)
                     l))]
      [(hash-ref (context-globals ctx) base-name #f)
       => (λ (g)
            (when (eq? (global-kind g) 'output)
              (zgal-error 'unknown-name stx "~a is an output of the ~a shader: give it with (results #:~a ...), as it can't be read"
                          base-name (context-stage ctx) base-name))
            (t-global (global-kind g) (global-name g) (global-spelling g) (global-type g) here))]
      [(hash-ref (context-constants ctx) base-name #f)
       => (λ (c)
            (set-add! (context-uses ctx) base-name)
            (t-global 'constant base-name base-name
                      (if (literal-constant? c (context-constants ctx)) literal-type (constant-decl-type c))
                      here))]
      [(hash-ref (hash-ref stage-variables (context-stage ctx) (hash)) base-name #f)
       => (λ (v) (t-global 'stage base-name (first v) (second v) here))]
      [(member base-name stage-variable-names)
       (zgal-error 'unknown-name stx "~a is a built-in variable of ~a~a" base-name
                   (if (eq? (context-stage ctx) 'app) "a shader stage, not of apps" "another stage")
                   (if (eq? (context-stage ctx) 'function) " (functions can't use stage variables: pass them as arguments)" ""))]
      [else (zgal-error 'unknown-name stx "~a isn't declared here" base-name)]))
  (for/fold ([e base]) ([p (in-list (rest parts))])
    (member-of e p stx)))

(define (check-form stx ctx locals)
  (define items (syntax->list stx))
  (define head (syntax-e (car items)))
  (define args (cdr items))
  (define here (stx->loc stx))
  (define (recur s [ls locals]) (check s ctx ls))
  (define (arity n-min n-max)
    (unless (and (>= (length args) n-min) (or (not n-max) (<= (length args) n-max)))
      (zgal-error 'malformed stx "(~a ...) takes ~a" head
                  (cond [(equal? n-min n-max) (format "~a argument~a" n-min (if (= n-min 1) "" "s"))]
                        [n-max (format "~a to ~a arguments" n-min n-max)]
                        [else (format "at least ~a arguments" n-min)]))))
  (define (bool-arg s what)
    (coerce (recur s) (scalar-t 'bool) s what))
  (define head-name (symbol->string head))
  (cond
    ;; .field and .swizzle as functions
    [(and (> (string-length head-name) 1) (char=? (string-ref head-name 0) #\.))
     (arity 1 1)
     (member-of (recur (first args)) (substring head-name 1) stx)]
    [else
     (case head
       [(let*) (check-let stx args ctx locals)]
       [(if)
        (arity 3 3)
        (define c (bool-arg (first args) "a condition"))
        (define a (recur (second args)))
        (define b (recur (third args)))
        (define-values (a2 b2 t) (unify-branches a b stx))
        (t-if c a2 b2 t here)]
       [(cond) (check-cond stx args ctx locals)]
       [(when unless)
        (arity 2 #f)
        (define c (bool-arg (first args) "a condition"))
        (define body (check-sequence (rest args) stx ctx locals))
        (t-if (if (eq? head 'when) c (t-op 'not (list c) (scalar-t 'bool) here)) body #f (void-t) here)]
       [(begin)
        (arity 1 #f)
        (check-sequence args stx ctx locals)]
       [(values)
        (arity 2 #f)
        (define es (map (λ (a) (settle (recur a))) args))
        (t-values es (values-t (map texpr-type es)) here)]
       [(fold) (check-fold stx args ctx locals #f)]
       [(while) (check-while stx args ctx locals)]
       [(sum) (check-fold stx args ctx locals #t)]
       [(for) (check-for stx args ctx locals)]
       [(results) (check-results stx args ctx locals)]
       [(with) (check-with stx args ctx locals)]
       [(at)
        (arity 2 2)
        (define base (recur (first args)))
        (define index (coerce-index (recur (second args)) (second args)))
        (define t
          (match (texpr-type base)
            [(array-t e _) e]
            [(runtime-array-t e) e]
            [(binding-array-t e) e]
            [(data-t e) e]
            [(vector-t k _) (scalar-t k)]
            [(matrix-t n) (vector-t 'float n)]
            [t (zgal-error 'type-mismatch (first args) "at indexes arrays, vectors and matrices, not ~a" (a-type t))]))
        (t-index base index t here)]
       [(count)
        (arity 1 1)
        (define base (recur (first args)))
        (unless (data-t? (texpr-type base))
          (zgal-error 'type-mismatch (first args) "count gives how many elements data has, and this is ~a" (a-type (texpr-type base))))
        (t-op 'data-count (list base) (scalar-t 'uint) here)]
       [(empty-data)
        (arity 1 1)
        (unless (eq? (context-stage ctx) 'app)
          (zgal-error 'wrong-stage stx "data is only in apps (shaders read buffers)"))
        (t-construct '() (data-t (check-type-name (first args) ctx)) here)]
       [(array-length)
        (arity 1 1)
        (define base (recur (first args)))
        (unless (binding-array-t? (texpr-type base))
          (zgal-error 'type-mismatch (first args) "array-length gives an array binding's number of elements, and this is ~a; a buffer-array's length isn't known to shaders (pass it as a push constant)"
                      (a-type (texpr-type base))))
        (t-op 'array-length (list base) (scalar-t 'uint) here)]
       [(+ - * /)
        (arity (if (eq? head '-) 1 2) (if (eq? head '/) 2 #f))
        (cond
          [(= (length args) 1)
           (define a (recur (first args)))
           (unless (numeric-type? (texpr-type a))
             (zgal-error 'type-mismatch stx "- negates numbers, not ~a" (a-type (texpr-type a))))
           (t-op 'negate (list a) (texpr-type a) here)]
          [else
           (for/fold ([acc (recur (first args))]) ([s (in-list (rest args))])
             (match-define (list a b t) (arithmetic head acc (recur s) stx))
             (t-op head (list a b) t here))])]
       [(remainder bitand bitor bitxor shl shr)
        (arity 2 (if (memq head '(remainder shl shr)) 2 #f))
        (for/fold ([acc (recur (first args))]) ([s (in-list (rest args))])
          (match-define (list a b t) (integer-op head acc (recur s) stx))
          (t-op head (list a b) t here))]
       [(bitnot)
        (arity 1 1)
        (define a (recur (first args)))
        (unless (or (literal? (texpr-type a)) (memq (component-kind (texpr-type a)) '(int uint)))
          (zgal-error 'type-mismatch stx "bitnot takes ints and uints, not ~a" (a-type (texpr-type a))))
        (t-op 'bitnot (list a) (texpr-type a) here)]
       [(< <= > >=)
        (arity 2 2)
        (match-define (list a b t) (arithmetic head (recur (first args)) (recur (second args)) stx))
        (unless (or (literal? t) (scalar-t? t))
          (zgal-error 'type-mismatch stx "~a compares numbers; compare vectors with lessThan, greaterThan, ..." head))
        (define operand (if (literal? t) (scalar-t 'int) t))
        (t-op head (list (coerce a operand stx "an operand") (coerce b operand stx "an operand")) (scalar-t 'bool) here)]
       [(= not=)
        (arity 2 2)
        (define a (recur (first args)))
        (define b (recur (second args)))
        (define-values (a2 b2 t) (unify-branches a b stx))
        (unless (or (scalar-t? t) (vector-t? t))
          (zgal-error 'type-mismatch stx "~a compares scalars and vectors, not ~a" head (a-type t)))
        (t-op head (list a2 b2) (scalar-t 'bool) here)]
       [(and or)
        (arity 1 #f)
        (t-op head (for/list ([a (in-list args)]) (bool-arg a (format "an operand of ~a" head))) (scalar-t 'bool) here)]
       [(not)
        (arity 1 1)
        (define a (recur (first args)))
        (match (texpr-type a)
          [(scalar-t 'bool) (t-op 'not (list a) (scalar-t 'bool) here)]
          [(vector-t 'bool n) (t-call 'not (list a) (vector-t 'bool n) here)]
          [t (zgal-error 'type-mismatch stx "not takes a bool or bool vector, not ~a" (a-type t))])]
       [(array)
        (arity 2 #f)
        (define element (check-type-name (first args) ctx))
        (t-construct (for/list ([a (in-list (rest args))]) (coerce (recur a) element a "an element"))
                     (array-t element (length (rest args))) here)]
       [(store!)
        (arity 3 3)
        (define image (recur (first args)))
        (unless (equal? (texpr-type image) (opaque-t 'image2D))
          (zgal-error 'type-mismatch (first args) "store! stores to a storage image, not ~a" (a-type (texpr-type image))))
        (t-store image (coerce (recur (second args)) (vector-t 'int 2) (second args) "a texel")
                 (coerce (recur (third args)) (vector-t 'float 4) (third args) "the stored value") here)]
       [(write!)
        (arity 2 2)
        (define place (check-raw (first args) ctx locals))
        (unless (writable-place? place)
          (zgal-error 'bad-write (first args) "write! writes to a buffer binding or shared variable, or a field or element of one; ~a isn't one"
                      (show (first args))))
        (t-write place (coerce (recur (second args)) (texpr-type place) (second args) "the written value") here)]
       [(sample sample-lod fetch sample-compare texture-size image-load image-size)
        (check-texture head stx args ctx locals)]
       [(print! set-frames-in-flight! resize! quit!)
        (unless (eq? (context-stage ctx) 'app)
          (zgal-error 'bad-effect stx "~a is an effect of apps, so only app handlers use it" head))
        (case head
          [(print!)
           (arity 1 #f)
           (t-effect 'print
                     (for/list ([a (in-list args)])
                       (cond
                         [(string? (syntax-e a)) (t-lit (syntax-e a) (opaque-t 'string) (stx->loc a))]
                         [else
                          (define e (settle (recur a)))
                          (unless (scalar-t? (texpr-type e))
                            (zgal-error 'type-mismatch a "print! prints strings and scalars, not ~a" (a-type (texpr-type e))))
                          e]))
                     here)]
          [(set-frames-in-flight!)
           (arity 1 1)
           (t-effect 'set-frames-in-flight (list (coerce (recur (first args)) (scalar-t 'uint) (first args) "the number of frames in flight")) here)]
          [(resize!)
           (arity 2 2)
           (t-effect 'resize (for/list ([a (in-list args)] [what (in-list '("the width" "the height"))])
                               (coerce (recur a) (scalar-t 'uint) a what))
                     here)]
          [(quit!)
           (arity 0 0)
           (t-effect 'quit '() here)])]
       [(atomic-add! atomic-min! atomic-max! atomic-and! atomic-or! atomic-xor! atomic-exchange! atomic-compare-exchange!)
        (define op (string->symbol (regexp-replace #px"^atomic-(.*)!$" (symbol->string head) "\\1")))
        (define compare? (eq? op 'compare-exchange))
        (arity (if compare? 3 2) (if compare? 3 2))
        (define place (check-raw (first args) ctx locals))
        (unless (and (writable-place? place) (not (t-swizzle? place)))
          (zgal-error 'bad-write (first args) "~a works on an element or field of a buffer binding or shared variable; ~a isn't one"
                      head (show (first args))))
        (define t (texpr-type place))
        (define kind (and (scalar-t? t) (scalar-t-kind t)))
        (unless (memq kind '(int uint float))
          (zgal-error 'type-mismatch (first args) "~a works on an int, uint or float, not ~a" head (a-type t)))
        (when (and (eq? kind 'float) (not (memq op '(add exchange))))
          (zgal-error 'type-mismatch (first args) "~a works on an int or uint; floats have atomic-add! and atomic-exchange!" head))
        (t-atomic op place
                  (for/list ([a (in-list (rest args))]) (coerce (recur a) t a "an atomic's operand"))
                  t here)]
       [(return! discard!)
        (arity 0 0)
        (define-values (op stage) (if (eq? head 'return!) (values 'return 'compute) (values 'discard 'fragment)))
        (unless (eq? (context-stage ctx) stage)
          (zgal-error 'wrong-stage stx "~a ends a ~a shader's invocation, so only ~a shaders use it" head stage stage))
        (t-exit op here)]
       [(barrier! buffer-barrier!)
        (arity 0 0)
        (unless (eq? (context-stage ctx) 'compute)
          (zgal-error 'bad-barrier stx "~a synchronizes a compute shader's workgroup, so only compute shaders (not ~a) use it" head
                      (if (eq? (context-stage ctx) 'function) "functions" (format "~a shaders" (context-stage ctx)))))
        (t-barrier (eq? head 'buffer-barrier!) here)]
       [else
        (cond
          [(hash-ref vector-constructors head #f) => (λ (t) (check-vector-constructor t stx args ctx locals))]
          [(hash-ref matrix-constructors head #f) => (λ (t) (check-matrix-constructor t stx args ctx locals))]
          [(hash-ref scalar-constructors head #f)
           => (λ (t)
                (arity 1 1)
                (define a (recur (first args)))
                (unless (or (scalar-t? (texpr-type a)) (literal? (texpr-type a)))
                  (zgal-error 'type-mismatch stx "~a converts a scalar, not ~a" head (a-type (texpr-type a))))
                ;; A literal is range checked; a constant of literals is converted like any int
                (if (and (small-integer-kind? (scalar-t-kind t)) (literal? (texpr-type a)) (not (t-global? a)))
                    (small-literal a t (first args))
                    (t-construct (list (settle a)) t here)))]
          [(hash-ref (context-structs ctx) head-name #f)
           => (λ (decl)
                (define fields (struct-decl-fields decl))
                (unless (= (length args) (length fields))
                  (zgal-error 'malformed stx "(~a ...) takes its ~a fields in order: ~a" head (length fields)
                              (string-join (map field-name fields) " ")))
                (t-construct (for/list ([a (in-list args)] [f (in-list fields)])
                               (coerce (recur a) (field-type f) a (format "field ~a" (field-name f))))
                             (struct-t decl) here))]
          [(hash-ref (context-functions ctx) head-name #f)
           => (λ (f)
                (define params (function-decl-params f))
                (unless (= (length args) (length params))
                  (zgal-error 'malformed stx "~a takes ~a argument~a" head (length params) (if (= (length params) 1) "" "s")))
                (define typed (for/list ([a (in-list args)] [p (in-list params)])
                                (coerce (recur a) (cdr p) a (format "argument ~a" (car p)))))
                (cond
                  [(inline-function? f) (inline-call f typed ctx here)]
                  [else
                   (set-add! (context-uses ctx) head-name)
                   (t-user-call head-name typed (function-decl-result f) here)]))]
          [(builtin-function? head)
           (when (and (derivative-function? head) (not (memq (context-stage ctx) '(fragment function))))
             (zgal-error 'wrong-stage stx "~a is a derivative across a fragment shader's neighbouring invocations, so only fragment shaders use it" head))
           (define typed (map recur args))
           (match (resolve-builtin head (map texpr-type typed))
             [(cons arg-types result)
              (t-call head (for/list ([a (in-list typed)] [t (in-list arg-types)]) (coerce a t stx "an argument")) result here)]
             [#f (zgal-error 'type-mismatch stx "~a isn't defined for ~a" head
                             (if (null? typed) "no arguments" (string-join (map (λ (a) (a-type (texpr-type a))) typed) ", ")))])]
          [else (zgal-error 'unknown-name (car items) "~a isn't a function, form or type" head)])])]))

(define (coerce-index e where)
  (define t (texpr-type e))
  (cond [(literal? t) (retype e (scalar-t 'int))]
        [(memq t (list (scalar-t 'int) (scalar-t 'uint))) e]
        [(member t (list (scalar-t 'int) (scalar-t 'uint))) e]
        [else (zgal-error 'type-mismatch where "an index is an int or uint, not ~a" (a-type t))]))

(define (writable-place? e)
  (match e
    [(t-global 'binding _ _ (or (struct-t _) (scalar-t _) (vector-t _ _) (matrix-t _) (runtime-array-t _)) _) #t]
    [(t-global 'shared _ _ _ _) #t]
    ;; An element of an array of buffers
    [(t-index (t-global 'binding _ _ (binding-array-t e) _) _ _ _) (not (opaque-t? e))]
    [(t-field b _ _ _) (writable-place? b)]
    [(t-index b _ _ _) (writable-place? b)]
    [(t-swizzle b _ _ _) (writable-place? b)]
    [_ #f]))

;; Unifies two branches' types (integer literals adapting)
(define (unify-branches a b where)
  (define ta (texpr-type a))
  (define tb (texpr-type b))
  (cond
    [(equal? ta tb) (values a b ta)]
    [(and (literal? ta) (scalar-t? tb) (numeric-kind? (scalar-t-kind tb))) (values (retype a tb) b tb)]
    [(and (literal? tb) (scalar-t? ta) (numeric-kind? (scalar-t-kind ta))) (values a (retype b ta) ta)]
    [else (zgal-error 'type-mismatch where "the branches are ~a and ~a" (a-type ta) (a-type tb))]))

;; A sequence (begin): every expression but the last is for its effects
(define (check-sequence stxs where ctx locals)
  (define es (for/list ([s (in-list stxs)]) (check s ctx locals)))
  (for ([e (in-list (drop-right es 1))] [s (in-list stxs)])
    (unless (or (void-t? (texpr-type e)) (discardable? e))
      (zgal-error 'type-mismatch s "only the last expression of a sequence has a value; this one is ~a" (a-type (texpr-type e)))))
  (if (= (length es) 1) (first es) (t-begin es (texpr-type (last es)) (stx->loc where))))

;; Whether e is for its effect, whatever its value: an atomic (or ends with one), whose previous
;; value a sequence may ignore
(define (discardable? e)
  (match e
    [(t-atomic _ _ _ _ _) #t]
    [(t-begin es _ _) (discardable? (last es))]
    [(t-let _ body _ _) (discardable? body)]
    [_ #f]))

(define (check-type-name stx ctx)
  (define d (syntax-e stx))
  (define name (and (symbol? d) (symbol->string d)))
  (cond
    [(and name (hash-ref (context-structs ctx) name #f)) => struct-t]
    [(and name (hash-ref vector-constructors d #f))]
    [(and name (hash-ref matrix-constructors d #f))]
    [(and name (hash-ref scalar-constructors d #f))]
    [else (zgal-error 'unknown-type stx "~a isn't a type" (show stx))]))

(define (check-let stx args ctx locals)
  (unless (>= (length args) 2)
    (zgal-error 'malformed stx "expected (let* ([name expression] ...) body ...)"))
  (define binding-stxs (or (syntax->list (first args)) (zgal-error 'malformed (first args) "expected ([name expression] ...)")))
  (define-values (bindings inner)
    (for/fold ([bindings '()] [ls locals] #:result (values (reverse bindings) ls))
              ([b (in-list binding-stxs)])
      (match (syntax->list b)
        [(list names-stx value-stx)
         (define value (settle (check value-stx ctx ls)))
         (define name-stxs (if (symbol? (syntax-e names-stx)) (list names-stx) (or (syntax->list names-stx) '())))
         (define types
           (match (texpr-type value)
             [(values-t ts) ts]
             [(void-t) (zgal-error 'type-mismatch value-stx "this expression has no value to bind")]
             [t (list t)]))
         (unless (= (length name-stxs) (length types))
           (zgal-error 'malformed b "~a name~a bound to ~a value~a" (length name-stxs) (if (= (length name-stxs) 1) "" "s")
                       (length types) (if (= (length types) 1) "" "s")))
         (cond
           ;; GLSL has no locals holding textures, images or buffers' arrays: the name stands for
           ;; the resource itself (e.g. an element of an array binding)
           [(and (= (length types) 1) (resource-type? (first types)))
            (define name (local-name (first name-stxs) ctx))
            (values bindings (hash-set ls name value))]
           [else
            (define-values (spelled ls2)
              (for/fold ([spelled '()] [ls ls] #:result (values (reverse spelled) ls))
                        ([n (in-list name-stxs)] [t (in-list types)])
                (define name (local-name n ctx))
                (define spelling (spell! ctx name))
                (values (cons spelling spelled) (hash-set ls name (t-local spelling t (stx->loc n))))))
            (values (cons (list spelled types value) bindings) ls2)])]
        [_ (zgal-error 'malformed b "expected [name expression] or [(name ...) expression]")])))
  (define body (check-sequence (rest args) stx ctx inner))
  (if (null? bindings) body (t-let bindings body (texpr-type body) (stx->loc stx))))

;; Types of resources rather than values: textures, samplers, images, buffers' arrays and arrays of
;; bindings
(define (resource-type? t)
  (or (opaque-t? t) (runtime-array-t? t) (binding-array-t? t)))

;; A local's name: valid, and not hiding a function, constant, struct or global
(define (local-name stx ctx)
  (define name (parse-name stx "a local name"))
  (when (or (hash-ref (context-functions ctx) name #f) (hash-ref (context-constants ctx) name #f)
            (hash-ref (context-structs ctx) name #f) (hash-ref (context-globals ctx) name #f)
            (member name stage-variable-names))
    (zgal-error 'duplicate-name stx "~a is already declared outside this body; locals can't hide it" name))
  name)

(define (check-cond stx args ctx locals)
  (when (null? args)
    (zgal-error 'malformed stx "expected (cond [condition expression] ... [else expression])"))
  (let loop ([clauses args])
    (match (syntax->list (car clauses))
      [(list c e) #:when (eq? (syntax-e c) 'else)
       (unless (null? (cdr clauses))
         (zgal-error 'malformed (car clauses) "else is the last clause of cond"))
       (check e ctx locals)]
      [(list c e)
       (when (null? (cdr clauses))
         (zgal-error 'malformed (car clauses) "cond ends with an [else expression] clause"))
       (define condition (coerce (check c ctx locals) (scalar-t 'bool) c "a condition"))
       (define-values (a b t) (unify-branches (check e ctx locals) (loop (cdr clauses)) (car clauses)))
       (t-if condition a b t (stx->loc (car clauses)))]
      [_ (zgal-error 'malformed (car clauses) "expected [condition expression]")])))

;; ([name (range from to [step])] ...): loops nested in order, each (list name from to step).
;; Returns the loops and the locals inside.
(define (check-loops stx ctx locals)
  (define loop-stxs (or (syntax->list stx) (zgal-error 'malformed stx "expected ([name (range from to)] ...)")))
  (when (null? loop-stxs)
    (zgal-error 'malformed stx "expected at least one [name (range from to)]"))
  (for/fold ([loops '()] [ls locals] #:result (values (reverse loops) ls))
            ([l (in-list loop-stxs)])
    (match (syntax->list l)
      [(list n r) #:when (eq? (head-symbol r) 'range)
       (match (syntax->list r)
         [(list _ from-stx to-stx step-stx ...)
          #:when (<= (length step-stx) 1)
          (define from (check from-stx ctx ls))
          (define to (check to-stx ctx ls))
          (define t (let ([tf (texpr-type from)] [tt (texpr-type to)])
                      (cond [(and (literal? tf) (literal? tt)) (scalar-t 'int)]
                            [(literal? tf) tt] [else tf])))
          (unless (member t (list (scalar-t 'int) (scalar-t 'uint)))
            (zgal-error 'type-mismatch r "a range is of ints or uints, not ~a" (a-type t)))
          (define step
            (cond
              [(null? step-stx) (t-lit 1 t (stx->loc r))]
              [else
               (define s (coerce (check (first step-stx) ctx ls) t (first step-stx) "a range's step"))
               (when (and (t-lit? s) (<= (t-lit-value s) 0))
                 (zgal-error 'malformed (first step-stx) "a range's step is positive, not ~a" (t-lit-value s)))
               s]))
          (define name (local-name n ctx))
          (define spelling (spell! ctx name))
          (values (cons (list spelling (coerce from t from-stx "a range's start") (coerce to t to-stx "a range's end") step) loops)
                  (hash-set ls name (t-local spelling t (stx->loc n))))]
         [_ (zgal-error 'malformed r "expected (range from to [step]): from up to, not including, to, by step (1 by default)")])]
      [_ (zgal-error 'malformed l "expected [name (range from to)]")])))

(define (check-fold stx args ctx locals sum?)
  (cond
    [sum?
     (unless (= (length args) 2)
       (zgal-error 'malformed stx "expected (sum ([name (range from to)] ...) expression)"))
     (define-values (loops inner) (check-loops (first args) ctx locals))
     (define body (settle (check (second args) ctx inner)))
     (define t (texpr-type body))
     (define acc (spell! ctx "sum"))
     ;; The body adds to the sum
     (define total (t-op '+ (list (t-local acc t (stx->loc stx)) body) t (stx->loc stx)))
     (t-fold (list (list acc t (zero t stx))) loops total t (stx->loc stx))]
    [else
     (unless (= (length args) 3)
       (zgal-error 'malformed stx "expected (fold ([accumulator initial] ...) ([name (range from to)] ...) expression)"))
     (define-values (accs acc-locals) (check-accumulators (first args) ctx locals))
     (define-values (loops inner) (check-loops (second args) ctx acc-locals))
     (define types (map second accs))
     (define body (next-values (check (third args) ctx inner) types (third args)))
     (t-fold accs loops body (if (= (length types) 1) (first types) (values-t types)) (stx->loc stx))]))

(define (check-for stx args ctx locals)
  (unless (>= (length args) 2)
    (zgal-error 'malformed stx "expected (for ([name (range from to)] ...) effect ...)"))
  (define-values (loops inner) (check-loops (first args) ctx locals))
  (define body (check-sequence (rest args) stx ctx inner))
  (unless (or (void-t? (texpr-type body)) (discardable? body))
    (zgal-error 'type-mismatch stx "for is for effects (write!, store!), but its body is ~a; use fold or sum for values"
                (a-type (texpr-type body))))
  (t-for loops body (stx->loc stx)))

(define (check-accumulators stx ctx locals)
  (define acc-stxs (or (syntax->list stx) (zgal-error 'malformed stx "expected ([accumulator initial] ...)")))
  (when (null? acc-stxs)
    (zgal-error 'malformed stx "a loop has at least one accumulator"))
  (define accs
    (for/list ([a (in-list acc-stxs)])
      (match (syntax->list a)
        [(list n init) (list n (settle (check init ctx locals)))]
        [_ (zgal-error 'malformed a "expected [accumulator initial]")])))
  (define-values (acc-locals spelled)
    (for/fold ([ls locals] [spelled '()] #:result (values ls (reverse spelled)))
              ([a (in-list accs)])
      (define name (local-name (first a) ctx))
      (define spelling (spell! ctx name))
      (values (hash-set ls name (t-local spelling (texpr-type (second a)) (stx->loc (first a)))) (cons spelling spelled))))
  (values (for/list ([a (in-list accs)] [s (in-list spelled)]) (list s (texpr-type (second a)) (second a)))
          acc-locals))

;; The next values of accumulators of types, from body
(define (next-values body types where)
  (match* (body types)
    [((t-values es _ l) _)
     (unless (= (length es) (length types))
       (zgal-error 'malformed where "the loop has ~a accumulators, so its body gives ~a values" (length types) (length types)))
     (t-values (for/list ([e (in-list es)] [t (in-list types)]) (coerce e t where "an accumulator's new value"))
               (values-t types) l)]
    [(_ (list t)) (coerce body t where "the accumulator's new value")]
    [(_ _) #:when (values-t? (texpr-type body))
     (unless (equal? (values-t-types (texpr-type body)) types)
       (zgal-error 'type-mismatch where "the loop's accumulators are ~a, but its body gives ~a"
                   (type-name (values-t types)) (type-name (texpr-type body))))
     body]
    [(_ _) (zgal-error 'malformed where "the loop has ~a accumulators, so its body is (values ...)" (length types))]))

;; (while ([accumulator initial] ...) condition next): while the condition holds (it sees the
;; accumulators), the accumulators take next's values. Its value is theirs at the end.
(define (check-while stx args ctx locals)
  (unless (= (length args) 3)
    (zgal-error 'malformed stx "expected (while ([accumulator initial] ...) condition expression)"))
  (define-values (accs inner) (check-accumulators (first args) ctx locals))
  (define condition (coerce (check (second args) ctx inner) (scalar-t 'bool) (second args) "a while's condition"))
  (define types (map second accs))
  (define body (next-values (check (third args) ctx inner) types (third args)))
  (t-while accs condition body (if (= (length types) 1) (first types) (values-t types)) (stx->loc stx)))

(define (check-with stx args ctx locals)
  (when (null? args)
    (zgal-error 'malformed stx "expected (with struct #:field value ...)"))
  (define base (check (first args) ctx locals))
  (define decl (match (texpr-type base)
                 [(struct-t d) d]
                 [t (zgal-error 'type-mismatch (first args) "with updates a struct, not ~a" (a-type t))]))
  (define fields
    (let loop ([rest (cdr args)] [acc '()])
      (match rest
        ['() (reverse acc)]
        [(list* k v more) #:when (keyword? (syntax-e k))
         (define name (keyword->string (syntax-e k)))
         (define f (or (for/first ([f (in-list (struct-decl-fields decl))] #:when (string=? (field-name f) name)) f)
                       (zgal-error 'unknown-name k "struct ~a has no field ~a" (struct-decl-name decl) name)))
         (loop more (cons (cons name (coerce (check v ctx locals) (field-type f) v (format "field ~a" name))) acc))]
        [_ (zgal-error 'malformed stx "expected (with struct #:field value ...)")])))
  (t-with base fields (struct-t decl) (stx->loc stx)))

;; (results #:name value ...): a stage's outputs, checked by the stage
(define (check-results stx args ctx locals)
  (define fields
    (let loop ([rest args] [acc '()])
      (match rest
        ['() (reverse acc)]
        [(list* k v more) #:when (keyword? (syntax-e k))
         (loop more (cons (list (keyword->string (syntax-e k)) (check v ctx locals) k) acc))]
        [_ (zgal-error 'malformed stx "expected (results #:name value ...)")])))
  ((context-results-checker ctx) fields stx))

;; How the stage checks results: set when checking a stage body
(define current-results-checker (make-parameter #f))
(define (context-results-checker ctx)
  (or (current-results-checker)
      (λ (fields stx) (zgal-error 'bad-results stx "results are a vertex or fragment shader's outputs, not ~a's"
                                  (if (eq? (context-stage ctx) 'function) "a function" (format "a ~a shader" (context-stage ctx)))))))

(define (check-texture op stx args ctx locals)
  (define (arg i) (check (list-ref args i) ctx locals))
  (define (opaque i kind)
    (define e (arg i))
    (unless (equal? (texpr-type e) (opaque-t kind))
      (zgal-error 'type-mismatch (list-ref args i) "~a takes a ~a here, not ~a" op kind (a-type (texpr-type e))))
    e)
  (define (typed i t what) (coerce (arg i) t (list-ref args i) what))
  (define (n k)
    (unless (= (length args) k)
      (zgal-error 'malformed stx "(~a ...) takes ~a arguments" op k)))
  (define here (stx->loc stx))
  (define vec2 (vector-t 'float 2))
  (define ivec2 (vector-t 'int 2))
  (define vec4 (vector-t 'float 4))
  (define float (scalar-t 'float))
  (define int (scalar-t 'int))
  (case op
    [(sample) (n 3) (t-texture op (list (opaque 0 'texture2D) (opaque 1 'sampler) (typed 2 vec2 "texture coordinates")) vec4 here)]
    [(sample-lod) (n 4) (t-texture op (list (opaque 0 'texture2D) (opaque 1 'sampler) (typed 2 vec2 "texture coordinates") (typed 3 float "a level of detail")) vec4 here)]
    [(fetch) (n 4) (t-texture op (list (opaque 0 'texture2D) (opaque 1 'sampler) (typed 2 ivec2 "a texel") (typed 3 int "a level")) vec4 here)]
    [(sample-compare) (n 4) (t-texture op (list (opaque 0 'texture2D) (opaque 1 'sampler) (typed 2 vec2 "texture coordinates") (typed 3 float "a reference depth")) float here)]
    [(texture-size) (n 2) (t-texture op (list (opaque 0 'texture2D) (typed 1 int "a level")) ivec2 here)]
    [(image-load) (n 2) (t-texture op (list (opaque 0 'image2D) (typed 1 ivec2 "a texel")) vec4 here)]
    [(image-size) (n 1) (t-texture op (list (opaque 0 'image2D)) ivec2 here)]))

(define (check-vector-constructor t stx args ctx locals)
  (define typed (for/list ([a (in-list args)]) (check a ctx locals)))
  (define kind (vector-t-kind t))
  (define converted
    (for/list ([e (in-list typed)] [s (in-list args)])
      (define et (texpr-type e))
      (cond
        [(literal? et) (retype e (scalar-t (if (eq? kind 'bool) 'int kind)))]
        [(or (scalar-t? et) (vector-t? et)) e]
        [else (zgal-error 'type-mismatch s "~a is made of scalars and vectors, not ~a" (type-name t) (a-type et))])))
  (define count (for/sum ([e (in-list converted)]) (components (texpr-type e))))
  (unless (or (= count (vector-t-size t)) (and (= (length converted) 1) (= count 1)))
    (zgal-error 'type-mismatch stx "~a needs ~a components (or one scalar for all of them), but has ~a"
                (type-name t) (vector-t-size t) count))
  (t-construct converted t (stx->loc stx)))

(define (check-matrix-constructor t stx args ctx locals)
  (define n (matrix-t-size t))
  (define typed (for/list ([a (in-list args)]) (check a ctx locals)))
  (define converted
    (for/list ([e (in-list typed)] [s (in-list args)])
      (define et (texpr-type e))
      (cond
        [(literal? et) (retype e (scalar-t 'float))]
        [(or (equal? et (scalar-t 'float)) (equal? et (vector-t 'float n)) (matrix-t? et)) e]
        [else (zgal-error 'type-mismatch s "~a is made of floats, vec~a columns or a matrix, not ~a" (type-name t) n (a-type et))])))
  (define count (for/sum ([e (in-list converted)]) (components (texpr-type e))))
  (unless (or (and (= (length converted) 1) (or (= count 1) (matrix-t? (texpr-type (first converted)))))
              (= count (* n n)))
    (zgal-error 'type-mismatch stx "~a needs ~a floats, ~a columns, one float (for its diagonal) or a matrix" (type-name t) (* n n) n))
  (t-construct converted t (stx->loc stx)))

;; ---------------------------------------------------------------------------------------------
;; Inlining

;; Functions taking storage images are inlined where they're called: Metal (through MoltenVK)
;; can't pass storage images to functions
(define (inline-function? f)
  (ormap (λ (p) (equal? (cdr p) (opaque-t 'image2D))) (function-decl-params f)))

;; f's body in place of a call with typed args: image arguments are substituted for their
;; parameters, other arguments bound to fresh locals, and the body's locals respelled in the
;; caller so they can't clash with its own
(define (inline-call f args ctx where)
  (for ([u (in-list (function-decl-uses f))]) (set-add! (context-uses ctx) u))
  (define mapping (make-hash))
  (define bindings
    (for/fold ([bindings '()] #:result (reverse bindings))
              ([p (in-list (function-decl-params f))] [a (in-list args)])
      (match-define (cons name type) p)
      (cond
        [(equal? type (opaque-t 'image2D)) (hash-set! mapping name a) bindings]
        [else
         (define fresh (spell! ctx (base-spelling name)))
         (hash-set! mapping name (t-local fresh type where))
         (cons (list (list fresh) (list type) a) bindings)])))
  (define body (respell (function-decl-body f) mapping ctx))
  (if (null? bindings) body (t-let bindings body (texpr-type body) where)))

;; A spelling without the _N spell! adds
(define (base-spelling s)
  (match (regexp-match #px"^(.*)_[0-9]+$" s)
    [(list _ base) base]
    [_ s]))

;; e with its locals renamed by mapping (spelling -> expression), giving binders fresh spellings
(define (respell e mapping ctx)
  (define (bind! name type l)
    (define fresh (spell! ctx (base-spelling name)))
    (hash-set! mapping name (t-local fresh type l))
    fresh)
  (define (go e) (respell e mapping ctx))
  (define (loops! ls l)
    (for/list ([lp (in-list ls)])
      (match-define (list name from to step) lp)
      ;; Bounds are in the enclosing scope, so they're renamed before the variable is bound
      (define from2 (go from))
      (define to2 (go to))
      (define step2 (go step))
      (list (bind! name (texpr-type from) l) from2 to2 step2)))
  (match e
    [(t-local name t l) (hash-ref mapping name (λ () e))]
    [(or (t-lit _ _ _) (t-global _ _ _ _ _) (t-barrier _ _)) e]
    [(t-effect op as l) (t-effect op (map go as) l)]
    [(t-atomic op p as t l) (t-atomic op (go p) (map go as) t l)]
    [(t-exit _ _) e]
    [(t-call f as t l) (t-call f (map go as) t l)]
    [(t-user-call f as t l) (t-user-call f (map go as) t l)]
    [(t-texture op as t l) (t-texture op (map go as) t l)]
    [(t-op op as t l) (t-op op (map go as) t l)]
    [(t-construct as t l) (t-construct (map go as) t l)]
    [(t-field b n t l) (t-field (go b) n t l)]
    [(t-swizzle b c t l) (t-swizzle (go b) c t l)]
    [(t-index b i t l) (t-index (go b) (go i) t l)]
    [(t-if c a b t l) (t-if (go c) (go a) (and b (go b)) t l)]
    [(t-let bindings body t l)
     (define bindings2
       (for/list ([b (in-list bindings)])
         (match-define (list names types value) b)
         (define value2 (go value))
         (list (for/list ([n (in-list names)] [ty (in-list types)]) (bind! n ty l)) types value2)))
     (t-let bindings2 (go body) t l)]
    [(t-fold accs loops body t l)
     (define inits (map (λ (a) (go (third a))) accs))
     (define accs2 (for/list ([a (in-list accs)] [init (in-list inits)]) (list (bind! (first a) (second a) l) (second a) init)))
     (define loops2 (loops! loops l))
     (t-fold accs2 loops2 (go body) t l)]
    [(t-for loops body l)
     (define loops2 (loops! loops l))
     (t-for loops2 (go body) l)]
    [(t-while accs c body t l)
     (define inits (map (λ (a) (go (third a))) accs))
     (define accs2 (for/list ([a (in-list accs)] [init (in-list inits)]) (list (bind! (first a) (second a) l) (second a) init)))
     (t-while accs2 (go c) (go body) t l)]
    [(t-values es t l) (t-values (map go es) t l)]
    [(t-begin es t l) (t-begin (map go es) t l)]
    [(t-store i tx v l) (t-store (go i) (go tx) (go v) l)]
    [(t-write p v l) (t-write (go p) (go v) l)]
    [(t-results fields l) (t-results (for/list ([f (in-list fields)]) (cons (car f) (go (cdr f)))) l)]
    [(t-with b fields t l) (t-with (go b) (for/list ([f (in-list fields)]) (cons (car f) (go (cdr f)))) t l)]))

;; ---------------------------------------------------------------------------------------------
;; What bodies do with bindings

;; The accesses of bindings in e: a hash from binding names to sets of 'read and 'write.
;; summaries: for each function, the accesses of its opaque parameters (a list of sets, by position).
(define (binding-accesses e summaries)
  (define accesses (make-hash))
  (define (note! name access) (hash-update! accesses name (λ (s) (set-add s access)) (set)))
  (define (root e)
    (match e
      [(t-global 'binding name _ _ _) name]
      [(or (t-field b _ _ _) (t-index b _ _ _) (t-swizzle b _ _ _)) (root b)]
      [_ #f]))
  ;; The binding an image or texture is, or is an element of
  (define (opaque-root e)
    (match e
      [(t-global 'binding name _ (opaque-t _) _) name]
      [(t-index (t-global 'binding name _ (binding-array-t (opaque-t _)) _) _ _ _) name]
      [_ #f]))
  (define (walk-indices e)
    (match e
      [(t-index _ i _ _) (walk i)]
      [_ (void)]))
  (define (walk e)
    (match e
      [(t-global 'binding name _ _ _) (note! name 'read)]
      [(t-write place value _)
       (define name (root place))
       (when name (note! name 'write))
       ;; Indices inside the place are read
       (let walk-place ([p place])
         (match p
           [(t-index b i _ _) (walk i) (walk-place b)]
           [(or (t-field b _ _ _) (t-swizzle b _ _ _)) (walk-place b)]
           [_ (void)]))
       (walk value)]
      [(t-atomic _ place args _ _)
       (define name (root place))
       (when name
         (note! name 'read)
         (note! name 'write))
       (let walk-place ([p place])
         (match p
           [(t-index b i _ _) (walk i) (walk-place b)]
           [(t-field b _ _ _) (walk-place b)]
           [_ (void)]))
       (for-each walk args)]
      [(t-store image texel value _)
       (match (opaque-root image)
         [#f (walk image)]
         [name (note! name 'write) (walk-indices image)])
       (walk texel)
       (walk value)]
      [(t-user-call f args _ _)
       (for ([a (in-list args)] [summary (in-list (hash-ref summaries f))])
         (match (opaque-root a)
           [#f (walk a)]
           [name (for ([access (in-set summary)]) (note! name access))
                 (walk-indices a)]))]
      [_ (for-each walk (texpr-children e))]))
  (walk e)
  accesses)

;; The accesses of a function's opaque parameters, by position (sets of 'read and 'write)
(define (parameter-accesses params body summaries)
  (define accesses (make-hash))
  (define (note! name access) (hash-update! accesses name (λ (s) (set-add s access)) (set)))
  (let walk ([e body])
    (match e
      [(t-local name (opaque-t _) _) (note! name 'read)]
      [(t-store (t-local name _ _) texel value _) (note! name 'write) (walk texel) (walk value)]
      [(t-user-call f args _ _)
       (for ([a (in-list args)] [summary (in-list (hash-ref summaries f))])
         (match a
           [(t-local name (opaque-t _) _) (for ([access (in-set summary)]) (note! name access))]
           [_ (walk a)]))]
      [_ (for-each walk (texpr-children e))]))
  (for/list ([p (in-list params)]) (hash-ref accesses (car p) (set))))

;; Whether e can run on the CPU too: no opaque values, texture operations, stage variables or
;; effects, and only calls to functions that can
(define (cpu-expression? e cpu-functions cpu-constants)
  (let ok? ([e e])
    (and (not (opaque-t? (texpr-type e)))
         (match e
           [(or (t-texture _ _ _ _) (t-store _ _ _ _) (t-write _ _ _) (t-barrier _ _) (t-effect _ _ _) (t-atomic _ _ _ _ _) (t-exit _ _)) #f]
           [(t-call f _ _ _) #:when (gpu-only-function? f) #f]
           [(t-global kind name _ _ _) (and (eq? kind 'constant) (set-member? cpu-constants name))]
           [(t-user-call f _ _ _) (and (set-member? cpu-functions f) (andmap ok? (texpr-children e)))]
           [_ (andmap ok? (texpr-children e))]))))

;; ---------------------------------------------------------------------------------------------
;; Barriers

;; Every invocation of a workgroup must reach a barrier, or none: checks body's barriers aren't
;; under conditions or in loops that could differ between invocations. Uniform values are
;; literals, constants, push constants, workgroupId and workgroupCount, elements at uniform
;; indices of buffers the body doesn't write, and what's computed only from them (shared memory and
;; buffers the body writes can change, so reading them isn't).
;; summaries: as binding-accesses takes, to know which bindings the body writes
(define (check-barriers body summaries)
  ;; Buffers the shader doesn't write hold the same values for the whole dispatch (only earlier work
  ;; wrote them), so reading one at a uniform index gives a uniform value, e.g. a count
  (define written
    (for/set ([(name access) (in-hash (binding-accesses body summaries))] #:when (set-member? access 'write)) name))
  (define (uniform? e locals)
    (match e
      [(t-lit _ _ _) #t]
      [(t-global 'binding name _ t _) (and (not (opaque-t? t)) (not (binding-array-t? t)) (not (set-member? written name)))]
      [(t-local name _ _) (set-member? locals name)]
      [(t-global (or 'constant 'push) _ _ _ _) #t]
      [(t-global 'stage name _ _ _) (and (member name '("workgroupId" "workgroupCount")) #t)]
      [(or (t-call _ _ _ _) (t-user-call _ _ _ _) (t-op _ _ _ _) (t-construct _ _ _) (t-field _ _ _ _)
           (t-swizzle _ _ _ _) (t-index _ _ _ _) (t-if _ _ _ _ _))
       (andmap (λ (c) (uniform? c locals)) (texpr-children e))]
      [_ #f]))
  (define (loops-uniform? loops locals)
    (for/fold ([ok locals]) ([l (in-list loops)])
      (match-define (list name from to step) l)
      (and ok (uniform? from ok) (uniform? to ok) (uniform? step ok) (set-add ok name))))
  ;; Walks e reached with divergence (why only some invocations may reach it, or #f), returning why
  ;; only some invocations may get past it: a return! only some take
  (define (walk e locals divergence)
    (define (go c) (walk c locals divergence))
    (define (sequence es locals divergence)
      (for/fold ([exited #f]) ([c (in-list es)])
        (or (walk c locals (or divergence exited)) exited)))
    (match e
      [(t-barrier _ l)
       (when divergence
         (zgal-error 'bad-barrier l "barrier! is ~a, so a workgroup's invocations could disagree about reaching it; every invocation must reach it, or none"
                     divergence))
       #f]
      [(t-exit 'return _)
       ;; Every invocation reaching it returns, so only a return some invocations skip matters
       (and divergence "after a return! only some invocations take")]
      [(t-begin es _ _) (sequence es locals divergence)]
      [(t-if c a b _ _)
       (go c)
       (define inner (or divergence (and (not (uniform? c locals)) "under a condition that can differ between invocations")))
       (or (walk a locals inner) (and b (walk b locals inner)))]
      [(t-let bindings body _ _)
       (define-values (inner exited)
         (for/fold ([ls locals] [exited #f]) ([b (in-list bindings)])
           (match-define (list names _ value) b)
           (define value-exited (walk value ls (or divergence exited)))
           (values (if (uniform? value ls) (for/fold ([ls ls]) ([n (in-list names)]) (set-add ls n)) ls)
                   (or value-exited exited))))
       (or (walk body inner (or divergence exited)) exited)]
      [(t-while accs c body _ _)
       (for ([a (in-list accs)]) (go (third a)))
       ;; Whether it continues depends on its accumulators, which can differ between invocations
       (define inner (or divergence "in a while loop, which can run a different number of times in each invocation"))
       (or (walk c locals inner) (walk body locals inner))]
      [(or (t-for loops body _) (t-fold _ loops body _ _))
       (for ([c (in-list (texpr-children e))] #:unless (eq? c body)) (go c))
       (define uniform-locals (loops-uniform? loops locals))
       (walk body (or uniform-locals locals)
             (or divergence (and (not uniform-locals) "in a loop whose range can differ between invocations")))]
      [_ (sequence (texpr-children e) locals divergence)]))
  (void (walk body (set) #f)))

;; ---------------------------------------------------------------------------------------------
;; App bodies

;; App bodies run on the CPU: checks e only calls functions in cpu-callable (a set of names)
(define (check-cpu-only e cpu-callable)
  (let walk ([e e])
    (match e
      [(t-user-call f _ _ l)
       (unless (set-member? cpu-callable f)
         (zgal-error 'gpu-only l "~a doesn't compile to C++ (it's GLSL, or calls GLSL), so only shaders can call it, not apps" f))]
      [_ (void)])
    (for-each walk (texpr-children e))))
