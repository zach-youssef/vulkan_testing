#lang racket/base
;; Reading and checking `graph` forms: render graphs, their resources, material instances and
;; nodes. Also the expressions graphs compute with, and their types, which backends use to emit them.

(require racket/list
         racket/match
         racket/string
         "ast.rkt"
         "builtins.rkt"
         "errors.rkt"
         "layout.rkt"
         "syntax.rkt")

(provide parse-graph
         (struct-out entry)
         graph-environment
         loop-environment
         expression-type
         per-frame-expression?
         unify
         image-formats
         depth-format?)

;; ---------------------------------------------------------------------------------------------
;; Names in a graph

;; kind: 'parameter 'constant 'per-frame 'meshes 'cpu 'extern 'path 'import (imported images and
;; image files) 'import-buffer 'sampler
;; 'uniform 'image 'buffer 'target 'material. type: the value's type (parameters, constants, per-frame inputs, uniforms,
;; buffers' elements), else #f. item: the declaration.
(struct entry (kind type item) #:transparent)

(define (item-entry item)
  (match item
    [(g-parameter _ t _) (entry 'parameter t item)]
    [(g-input _ kind t _) (entry kind t item)]
    [(g-import _ _ _) (entry 'import #f item)]
    ;; An imported image, which the generated code imports
    [(g-image-file _ _ _ _) (entry 'import #f item)]
    [(g-model _ _ _ _) (entry 'model #f item)]
    ;; A set of images, like imported ones, and an uploaded buffer
    [(g-model-textures _ _ _) (entry 'import-images #f item)]
    [(g-model-factors _ _ _) (entry 'upload (vector-t 'float 4) item)]
    [(g-model-draws _ _ t _) (entry 'upload t item)]
    [(g-model-bounds _ _ _) (entry 'upload (vector-t 'float 4) item)]
    [(g-import-images _ _ _) (entry 'import-images #f item)]
    [(g-import-buffer _ t _) (entry 'import-buffer t item)]
    [(g-upload _ _ t _) (entry 'upload t item)]
    ;; Like an upload, with its elements from files
    [(g-buffer-file _ t _ _) (entry 'upload t item)]
    [(g-constant _ _ t _) (entry 'constant t item)]
    [(g-sampler _ _ _) (entry 'sampler #f item)]
    [(g-uniform _ t _ _) (entry 'uniform t item)]
    [(g-image _ _ _ _ _ _) (entry 'image #f item)]
    [(g-buffer _ t _ _ _ _) (entry 'buffer t item)]
    [(g-target _ _ _ _) (entry 'target #f item)]
    [(g-material _ _ _ _ _ _ _ _) (entry 'material #f item)]))

;; The graph's names: a hash from names to entries
(define (graph-environment g)
  (for/hash ([item (in-list (graph-items g))]) (values (graph-item-name item) (item-entry item))))

;; env with the variables of loops (g-loops' clauses, in order), e.g. those a node is in
(define (loop-environment env loops)
  (for*/fold ([env env]) ([l (in-list loops)] [c (in-list (g-loop-clauses l))])
    (hash-set env (loop-clause-name c) (entry 'loop (loop-clause-type c) c))))

(define kind-names
  (hash 'parameter "a parameter" 'constant "a constant" 'per-frame "a per-frame input" 'meshes "a meshes input"
        'cpu "a cpu input" 'extern "an extern input" 'path "a path input" 'model "a model" 'per-frame-data "a per-frame data input" 'import "an imported image" 'import-images "imported images" 'import-buffer "an imported buffer" 'data "a data input" 'upload "an uploaded buffer" 'sampler "a sampler" 'uniform "a uniform"
        'image "an image" 'buffer "a buffer" 'target "a target" 'material "a material" 'loop "a loop variable"))

;; ---------------------------------------------------------------------------------------------
;; Expressions and their types


(define (numeric-scalar? t) (and (scalar-t? t) (memq (scalar-t-kind t) '(literal int uint float)) #t))
(define (numeric? t)
  (or (numeric-scalar? t) (and (vector-t? t) (memq (vector-t-kind t) '(int uint float)) #t)))
(define bool-t (scalar-t 'bool))

(define (describe-type t) (if (equal? t literal-type) "integer literal" (type-name t)))
;; The type with an article: "a uint", "an int", "an integer literal"
(define (a-type t)
  (define d (describe-type t))
  (string-append (if (memv (string-ref d 0) '(#\a #\e #\i #\o)) "an " "a ") d))

;; The type two operands share, or #f
(define (unify a b)
  (cond
    [(equal? a b) a]
    [(and (equal? a literal-type) (scalar-t? b) (memq (scalar-t-kind b) '(int uint float))) b]
    [(and (equal? b literal-type) (scalar-t? a) (memq (scalar-t-kind a) '(int uint float))) a]
    [else #f]))

(define (unify-or-fail a b where what)
  (or (unify a b)
      (zgal-error 'type-mismatch where "~a: ~a and ~a don't match" what (describe-type a) (describe-type b))))

(define vector-constructors
  (for*/hash ([kind '(float int uint)] [n '(2 3 4)])
    (define t (vector-t kind n))
    (values (string->symbol (type-name t)) t)))

;; The type of e. env: the graph's names (so far). per-frame-ok?: whether per-frame inputs may be
;; used (else the expression is computed when the graph is built).
(define (expression-type e env #:per-frame-ok? [per-frame-ok? #t])
  (define (recur e) (expression-type e env #:per-frame-ok? per-frame-ok?))
  (define (arity! n-min n-max args op where)
    (unless (and (>= (length args) n-min) (or (not n-max) (<= (length args) n-max)))
      (zgal-error 'malformed where "(~a ...) takes ~a" op
                  (cond [(equal? n-min n-max) (format "~a argument~a" n-min (if (= n-min 1) "" "s"))]
                        [n-max (format "~a to ~a arguments" n-min n-max)]
                        [else (format "at least ~a arguments" n-min)]))))
  (define (require-type t ok? where what)
    (unless (ok? t)
      (zgal-error 'type-mismatch where "~a, not ~a" what (a-type t)))
    t)
  (match e
    [(e-literal v _)
     (cond [(boolean? v) bool-t]
           [(exact-integer? v) literal-type]
           [else (scalar-t 'float)])]
    [(e-ref name where)
     (match (hash-ref env name #f)
       [#f (zgal-error 'unknown-name where "~a isn't declared in this graph before here" name)]
       [(entry (or 'parameter 'constant 'loop) t _) t]
       [(entry 'per-frame t _)
        (unless per-frame-ok?
          (zgal-error 'not-build-time where
                      "~a is a per-frame input, but this value is computed once, when the graph is built" name))
        t]
       [(entry kind _ _)
        (zgal-error 'type-mismatch where "~a is ~a, not a value" name (hash-ref kind-names kind))])]
    [(e-field base field where)
     (define t (recur base))
     (match t
       [(struct-t d)
        (define f (or (for/first ([f (in-list (struct-decl-fields d))] #:when (string=? (field-name f) field)) f)
                      (zgal-error 'unknown-name where "~a has no field ~a" (type-name t) field)))
        (when (array-t? (field-type f))
          (zgal-error 'type-not-allowed where "field ~a of ~a is an array, which graphs' expressions don't take" field (type-name t)))
        (field-type f)]
       [(vector-t k n)
        (define components (or (swizzle-components field n)
                               (zgal-error 'unknown-name where "~a isn't a swizzle of a ~a (use up to four of xyzw, rgba or stpq)"
                                           field (type-name t))))
        (if (= (string-length components) 1) (scalar-t k) (vector-t k (string-length components)))]
       [_ (zgal-error 'type-mismatch where "~a is ~a, which has no fields" field (a-type t))])]
    [(e-op op args where)
     (define (args-types) (map recur args))
     (case op
       [(if)
        (arity! 3 3 args op where)
        (require-type (recur (first args)) (λ (t) (equal? t bool-t)) (e-loc (first args)) "a condition is a bool")
        (unify-or-fail (recur (second args)) (recur (third args)) where "the branches of if")]
       [(and or)
        (arity! 1 #f args op where)
        (for ([a (in-list args)])
          (require-type (recur a) (λ (t) (equal? t bool-t)) (e-loc a) (format "~a takes bools" op)))
        bool-t]
       [(not)
        (arity! 1 1 args op where)
        (require-type (recur (first args)) (λ (t) (equal? t bool-t)) (e-loc (first args)) "not takes a bool")]
       [(= < <= > >=)
        (arity! 2 2 args op where)
        (define t (unify-or-fail (recur (first args)) (recur (second args)) where (format "the operands of ~a" op)))
        (unless (or (numeric-scalar? t) (and (eq? op '=) (equal? t bool-t)))
          (zgal-error 'type-mismatch where "~a compares numbers, not ~a" op (a-type t)))
        bool-t]
       [(+ - * / min max)
        (arity! (if (eq? op '-) 1 2) (if (memq op '(/ min max)) 2 #f) args op where)
        (define t (for/fold ([t (recur (first args))]) ([a (in-list (rest args))])
                    (unify-or-fail t (recur a) where (format "the operands of ~a" op))))
        (require-type t numeric? where (format "~a takes numbers or numeric vectors" op))]
       [(float int uint)
        (arity! 1 1 args op where)
        (require-type (recur (first args)) numeric-scalar? (e-loc (first args)) (format "~a converts a number" op))
        (scalar-t op)]
       [(count)
        (arity! 1 1 args op where)
        (match (first args)
          [(e-ref name ref-where)
           (match (hash-ref env name #f)
             [(entry 'data _ _) (scalar-t 'uint)]
             [(entry 'per-frame-data _ _)
              (unless per-frame-ok?
                (zgal-error 'not-build-time where
                            "~a is a per-frame data input, whose count is only known each frame, but this value is computed once, when the graph is built" name))
              (scalar-t 'uint)]
             [_ (zgal-error 'type-mismatch ref-where "(count input) takes a data input, and ~a isn't one" name)])]
          [other (zgal-error 'malformed (e-loc other) "(count input) takes a data input's name")])]
       [(at)
        (arity! 2 2 args op where)
        (require-type (recur (second args)) (λ (t) (and (scalar-t? t) (memq (scalar-t-kind t) '(literal int uint)) #t))
                      (e-loc (second args)) "an element's index is an integer")
        (match (first args)
          [(e-ref name ref-where)
           (match (hash-ref env name #f)
             [(entry 'data t _) t]
             [_ (zgal-error 'type-mismatch ref-where "(at input index) takes a data input, and ~a isn't one" name)])]
          [other (zgal-error 'malformed (e-loc other) "(at input index) takes a data input's name")])]
       [(width height)
        (arity! 1 1 args op where)
        (match (first args)
          [(e-ref name ref-where)
           (match (hash-ref env name #f)
             ;; Known when the graph is built
             [(entry 'import _ _) (scalar-t 'uint)]
             ;; Graph images can be recreated (e.g. surface-relative ones, when the surface resizes)
             [(entry 'image _ _)
              (unless per-frame-ok?
                (zgal-error 'not-build-time where
                            "~a is a graph image, whose size is only known each frame (it can be recreated, e.g. when the surface resizes), but this value is computed once, when the graph is built"
                            name))
              (scalar-t 'uint)]
             [_ (zgal-error 'type-mismatch ref-where "(~a image) takes an image, and ~a isn't one" op name)])]
          [other (zgal-error 'malformed (e-loc other) "(~a image) takes an image's name" op)])]
       [else
        (define t (hash-ref vector-constructors op #f))
        (unless t
          (zgal-error 'malformed where "unknown operation ~a" op))
        (arity! (vector-t-size t) (vector-t-size t) args op where)
        (for ([a (in-list args)])
          (unify-or-fail (recur a) (scalar-t (vector-t-kind t)) (e-loc a) (format "a component of ~a" op)))
        t])]))

(define (e-loc e)
  (match e [(e-literal _ l) l] [(e-ref _ l) l] [(e-op _ _ l) l] [(e-field _ _ l) l]))

;; Whether e can change from frame to frame: it uses per-frame inputs or the size of a graph image
(define (per-frame-expression? e env)
  (match e
    [(e-ref name _) (match (hash-ref env name #f) [(entry 'per-frame _ _) #t] [_ #f])]
    [(e-op (or 'width 'height) (list (e-ref name _)) _) (match (hash-ref env name #f) [(entry 'image _ _) #t] [_ #f])]
    [(e-op 'count (list (e-ref name _)) _) (match (hash-ref env name #f) [(entry 'per-frame-data _ _) #t] [_ #f])]
    [(e-op _ args _) (ormap (λ (a) (per-frame-expression? a env)) args)]
    [(e-field base _ _) (per-frame-expression? base env)]
    [_ #f]))

;; Checks e has type `expected` (integer literals adapting to it)
(define (check-expression e env expected what #:per-frame-ok? [per-frame-ok? #t])
  (define t (expression-type e env #:per-frame-ok? per-frame-ok?))
  (unless (equal? (unify t expected) expected)
    (zgal-error 'type-mismatch (e-loc e) "~a is ~a, not ~a" what (a-type expected) (a-type t))))

(define (parse-expression stx)
  (define d (syntax-e stx))
  (cond
    [(or (boolean? d) (and (real? d) (not (and (inexact? d) (or (nan? d) (infinite? d))))))
     (e-literal d (stx->loc stx))]
    [(symbol? d)
     ;; A name, followed by fields and components after dots: settings.gain, pass.offset.x
     (define parts (string-split (symbol->string d) "." #:trim? #f))
     (when (ormap (λ (p) (string=? p "")) parts)
       (zgal-error 'malformed stx "~a isn't a name: write fields as name.field" d))
     (for/fold ([e (e-ref (first parts) (stx->loc stx))]) ([f (in-list (rest parts))])
       (e-field e f (stx->loc stx)))]
    [(and (syntax->list stx) (pair? (syntax->list stx)) (symbol? (syntax-e (car (syntax->list stx)))))
     (define items (syntax->list stx))
     (define head (symbol->string (syntax-e (car items))))
     (cond
       ;; (.field expression)
       [(and (> (string-length head) 1) (char=? (string-ref head 0) #\.))
        (unless (= (length items) 2)
          (zgal-error 'malformed stx "(~a expression) takes one expression" head))
        (e-field (parse-expression (second items)) (substring head 1) (stx->loc stx))]
       [else (e-op (syntax-e (car items)) (map parse-expression (cdr items)) (stx->loc stx))])]
    [else (zgal-error 'malformed stx "expected an expression, found ~a" (show stx))]))

(define (nan? x) (not (= x x)))
(define (infinite? x) (and (= x (* 2 x)) (not (zero? x))))

;; ---------------------------------------------------------------------------------------------
;; Formats and other choices

(define image-formats
  (hash 'r8 "VK_FORMAT_R8_UNORM" 'rg8 "VK_FORMAT_R8G8_UNORM" 'rgba8 "VK_FORMAT_R8G8B8A8_UNORM"
        'rgba8-srgb "VK_FORMAT_R8G8B8A8_SRGB" 'bgra8 "VK_FORMAT_B8G8R8A8_UNORM" 'bgra8-srgb "VK_FORMAT_B8G8R8A8_SRGB"
        'r16f "VK_FORMAT_R16_SFLOAT" 'rg16f "VK_FORMAT_R16G16_SFLOAT" 'rgba16f "VK_FORMAT_R16G16B16A16_SFLOAT"
        'r32f "VK_FORMAT_R32_SFLOAT" 'rg32f "VK_FORMAT_R32G32_SFLOAT" 'rgba32f "VK_FORMAT_R32G32B32A32_SFLOAT"
        'r32ui "VK_FORMAT_R32_UINT" 'r32i "VK_FORMAT_R32_SINT"
        'd16 "VK_FORMAT_D16_UNORM" 'd32 "VK_FORMAT_D32_SFLOAT" 'd24s8 "VK_FORMAT_D24_UNORM_S8_UINT"
        'd32s8 "VK_FORMAT_D32_SFLOAT_S8_UINT"))

(define (depth-format? f) (and (memq f '(d16 d32 d24s8 d32s8)) #t))

(define (parse-format stx)
  (define f (syntax-e stx))
  (unless (hash-ref image-formats f #f)
    (zgal-error 'unknown-format stx "unknown image format ~a; formats are ~a" (show stx)
                (string-join (sort (map symbol->string (hash-keys image-formats)) string<?) ", ")))
  f)

(define compare-ops '(never less equal less-or-equal greater not-equal greater-or-equal always))

;; ---------------------------------------------------------------------------------------------
;; Library kernels

;; What a kernel template's operator symbols become for op ('add 'min 'max) on elements of type t
;; (a uint, int or float scalar-t): see dsl/lib/kernels/reduce.zgal
(define (operator-substitutions op t)
  (define kind (scalar-t-kind t))
  (define identity
    (match* (op kind)
      [('add 'float) 0.0] [('add k) `(,k 0)]
      [('min 'uint) '(bitnot (uint 0))] [('min 'int) '(int 2147483647)] [('min 'float) '(uintBitsToFloat (uint 2139095040))]
      [('max 'uint) '(uint 0)] [('max 'int) '(- (int -2147483647) 1)] [('max 'float) '(uintBitsToFloat (uint 4286578688))]))
  (define title (case op [(add) "Add"] [(min) "Min"] [(max) "Max"]))
  (hash 'T kind
        'COMBINE (case op [(add) '+] [else op])
        'IDENTITY identity
        'SUBGROUP_ALL (string->symbol (format "subgroup~a" title))
        'SUBGROUP_INCLUSIVE (string->symbol (format "subgroupInclusive~a" title))
        'SUBGROUP_EXCLUSIVE (string->symbol (format "subgroupExclusive~a" title))))

;; ---------------------------------------------------------------------------------------------
;; The graph

;; stx: the (graph Name item ...) form. parse-type: parses a type. top-level: declares a top-level
;; name. materials: the file's materials so far, by name. instantiate-kernel: instantiates a library
;; kernel's materials (see parse.rkt).
(define (parse-graph stx parse-type top-level materials formats instantiate-kernel)
  (define items (syntax->list stx))
  (unless (and items (>= (length items) 2))
    (zgal-error 'malformed stx "expected (graph Name item ...)"))
  (define name (parse-name (second items) "the graph's name"))
  (top-level name (second items))
  (define names (make-namespace (format "in graph ~a" name)))
  (define node-names (make-namespace (format "as a node of graph ~a" name)))
  (define env (make-hash))   ; name -> entry, as declared so far
  ;; Items nodes declare (e.g. a kernel's scratch buffers and materials), in order
  (define node-items '())
  ;; The loops the node being read is in, outermost first
  (define current-loops '())
  (define (declare! item stx)
    (names (graph-item-name item) stx)
    (hash-set! env (graph-item-name item) (item-entry item))
    item)
  (define (lookup stx kinds what)
    (define n (syntax-e stx))
    (unless (symbol? n)
      (zgal-error 'malformed stx "expected the name of ~a, found ~a" what (show stx)))
    (match (hash-ref env (symbol->string n) #f)
      [#f (zgal-error 'unknown-name stx "~a isn't declared in graph ~a before here" n name)]
      [(entry kind _ item) #:when (memq kind kinds) (symbol->string n)]
      [(entry kind _ _) (zgal-error 'type-mismatch stx "~a is ~a, not ~a" n (hash-ref kind-names kind) what)]))
  (define (build-time stx type what)
    (define e (parse-expression stx))
    (check-expression e env type what #:per-frame-ok? #f)
    e)
  (define (condition options key)
    (define stx (hash-ref options key #f))
    (and stx (build-time stx (scalar-t 'bool) (format "~a" key))))
  (define (frames options form where)
    (choice-option options '#:frames '(shared per-frame) form #:required? #t #:where where))

  ;; Models fill a vertex format's attributes by name, or with their defaults
  (define (check-model-format fmt where)
    (for ([a (in-list (vertex-format-attributes fmt))])
      (define m (model-attribute-of a))
      (define t (field-type a))
      (cond
        [m
         (define size (cond [(vector-t? t) (and (eq? (vector-t-kind t) 'float) (vector-t-size t))] [else #f]))
         (unless (memv size (model-attribute-sizes m))
           (zgal-error 'model-attribute where "attribute ~a of ~a takes models' ~a, so it's ~a, not ~a"
                       (field-name a) (vertex-format-name fmt) (model-attribute-member m)
                       (string-join (for/list ([n (in-list (model-attribute-sizes m))]) (format "vec~a" n)) " or ")
                       (type-name t)))]
        [(not (attribute-default a))
         (zgal-error 'model-attribute where "models don't give attribute ~a of ~a: name it after what they give (~a) or give it a #:default"
                     (field-name a) (vertex-format-name fmt)
                     (string-join (for/list ([m (in-list model-attributes)]) (first (model-attribute-names m))) ", "))])))

  ;; An asset's path: a string, or a path input's name
  (define (parse-asset-source stx)
    (define d (syntax-e stx))
    (cond
      [(string? d)
       (when (string=? d "")
         (zgal-error 'malformed stx "an asset's path isn't empty"))
       (asset-source d #f)]
      [(symbol? d) (asset-source #f (lookup stx '(path) "a path input"))]
      [else (zgal-error 'malformed stx "expected an asset's path, a string or a path input, found ~a" (show stx))]))

  (define (parse-item stx)
    (define parts (syntax->list stx))
    (unless (and parts (pair? parts) (symbol? (syntax-e (car parts))))
      (zgal-error 'malformed stx "expected a graph item (kind name ...), found ~a" (show stx)))
    (define head (syntax-e (car parts)))
    (define form (symbol->string head))
    (define-values (args options) (split-options (cdr parts) form))
    (define (args! n shape)
      (unless (= (length args) n)
        (zgal-error 'malformed stx "expected ~a" shape)))
    (define (item-name) (parse-name (first args) (format "the ~a's name" form)))
    (match head
      ['parameter
       (args! 2 "(parameter name type)")
       (check-known-options options '() form)
       (define t (parse-type (second args)))
       (unless (and (scalar-t? t) (memq (scalar-t-kind t) '(bool int uint float)))
         (zgal-error 'type-not-allowed (second args) "a parameter is a bool, int, uint or float, not ~a" (type-name t)))
       (declare! (g-parameter (item-name) t (stx->loc stx)) stx)]
      ['input
       (args! 2 "(input name (per-frame type)), (input name (data type)), (input name (per-frame (data type))), (input name meshes), (input name cpu), (input name extern) or (input name path)")
       (check-known-options options '() form)
       (define kind-stx (second args))
       (define item
         (match (syntax-e kind-stx)
           ['meshes (g-input (item-name) 'meshes #f (stx->loc stx))]
           ['cpu (g-input (item-name) 'cpu #f (stx->loc stx))]
           ['extern (g-input (item-name) 'extern #f (stx->loc stx))]
           ['path (g-input (item-name) 'path #f (stx->loc stx))]
           [_
            (match (syntax->list kind-stx)
              ;; Elements each frame: re-uploaded when they change
              [(list k data-stx) #:when (and (eq? (syntax-e k) 'per-frame) (eq? (head-symbol data-stx) 'data))
               (match (syntax->list data-stx)
                 [(list _ t-stx) (g-input (item-name) 'per-frame-data (buffer-element-type t-stx "a data input") (stx->loc stx))]
                 [_ (zgal-error 'malformed data-stx "expected (data type)")])]
              [(list k t-stx) #:when (eq? (syntax-e k) 'per-frame)
               (define t (parse-type t-stx))
               (when (array-t? t)
                 (zgal-error 'type-not-allowed t-stx "a per-frame input isn't an array; use a struct holding one"))
               (g-input (item-name) 'per-frame t (stx->loc stx))]
              [(list k t-stx) #:when (eq? (syntax-e k) 'data)
               (g-input (item-name) 'data (buffer-element-type t-stx "a data input") (stx->loc stx))]
              [_ (zgal-error 'malformed kind-stx "an input is (per-frame type), (data type), (per-frame (data type)), meshes, cpu, extern or path, not ~a" (show kind-stx))])]))
       (declare! item stx)]
      ['import-image
       (args! 1 "(import-image name #:resting state)")
       (check-known-options options '(#:resting) form)
       (define resting (choice-option options '#:resting '(shader-read-only general transfer-src) form
                                      #:required? #t #:where stx))
       (declare! (g-import (item-name) resting (stx->loc stx)) stx)]
      ['image-file
       (args! 2 "(image-file name source #:color color)")
       (check-known-options options '(#:color) form)
       (define color (choice-option options '#:color '(srgb linear) form #:required? #t #:where stx))
       (declare! (g-image-file (item-name) (parse-asset-source (second args)) color (stx->loc stx)) stx)]
      ['model
       (args! 3 "(model name asset Format #:base-colors name #:base-color-factors name #:draws name #:bounds name)")
       (check-known-options options '(#:base-colors #:base-color-factors #:draws #:bounds) form)
       (define model-name (item-name))
       (define source (parse-asset-source (second args)))
       (define format-name (syntax-e (third args)))
       (define fmt
         (or (and (symbol? format-name) (hash-ref formats (symbol->string format-name) #f))
             (zgal-error 'unknown-name (third args) "~a isn't a vertex format declared earlier in the file" (show (third args)))))
       (check-model-format fmt (third args))
       (define model (declare! (g-model model-name source fmt (stx->loc stx)) stx))
       ;; Its materials' base colours, under the names given
       (define (declare-part! key make)
         (define part-stx (hash-ref options key #f))
         (and part-stx (declare! (make (parse-name part-stx (format "~a's ~a" form key)) model-name (stx->loc part-stx)) part-stx)))
       ;; Its draws, as the commands of (import indirect)
       (define draws
         (let ([draws-stx (hash-ref options '#:draws #f)])
           (and draws-stx
                (let ([t (with-handlers ([exn:zgal? (λ (_) (zgal-error 'unknown-type draws-stx "a model's #:draws are DrawIndexedCommands: (import indirect)"))])
                           (parse-type (datum->syntax draws-stx 'DrawIndexedCommand draws-stx)))])
                  (check-command-type! t "DrawIndexedCommand" draws-stx "a model's #:draws")
                  (declare-part! '#:draws (λ (n m l) (g-model-draws n m t l)))))))
       (filter values (list model (declare-part! '#:base-colors g-model-textures)
                            (declare-part! '#:base-color-factors g-model-factors)
                            draws
                            (declare-part! '#:bounds g-model-bounds)))]
      ['import-images
       (args! 1 "(import-images name #:resting state)")
       (check-known-options options '(#:resting) form)
       (define resting (choice-option options '#:resting '(shader-read-only general transfer-src) form
                                      #:required? #t #:where stx))
       (declare! (g-import-images (item-name) resting (stx->loc stx)) stx)]
      ['import-buffer
       (args! 2 "(import-buffer name type)")
       (check-known-options options '() form)
       (declare! (g-import-buffer (item-name) (buffer-element-type (second args) form) (stx->loc stx)) stx)]
      ['upload
       (args! 2 "(upload name data-input)")
       (check-known-options options '() form)
       (define source (lookup (second args) '(data per-frame-data) "a data input"))
       (declare! (g-upload (item-name) source (entry-type (hash-ref env source)) (stx->loc stx)) stx)]
      ['buffer-file
       (unless (>= (length args) 3)
         (zgal-error 'malformed stx "expected (buffer-file name type asset ...+)"))
       (check-known-options options '() form)
       (declare! (g-buffer-file (item-name) (buffer-element-type (second args) form)
                                (map parse-asset-source (cddr args)) (stx->loc stx))
                 stx)]
      ['constant
       (args! 2 "(constant name expression)")
       (check-known-options options '() form)
       (define e (parse-expression (second args)))
       (define t (let ([t (expression-type e env #:per-frame-ok? #f)])
                   (if (equal? t literal-type) (scalar-t 'int) t)))
       (declare! (g-constant (item-name) e t (stx->loc stx)) stx)]
      ['sampler
       (args! 1 "(sampler name #:option value ...)")
       (check-known-options options '(#:filter #:mipmap #:address #:border #:compare #:anisotropy #:max-lod) form)
       (define (choice key choices) (choice-option options key choices form))
       (define max-lod (hash-ref options '#:max-lod #f))
       (when (and max-lod (not (and (real? (syntax-e max-lod)) (>= (syntax-e max-lod) 0))))
         (zgal-error 'bad-option max-lod "sampler's #:max-lod is a non-negative number"))
       (define anisotropy (hash-ref options '#:anisotropy #f))
       (when (and anisotropy (not (boolean? (syntax-e anisotropy))))
         (zgal-error 'bad-option anisotropy "sampler's #:anisotropy is #t or #f"))
       (define settings
         (filter cdr
                 (list (cons 'filter (choice '#:filter '(linear nearest)))
                       (cons 'mipmap (choice '#:mipmap '(linear nearest)))
                       (cons 'address (choice '#:address '(repeat mirrored-repeat clamp-to-edge clamp-to-border)))
                       (cons 'border (choice '#:border '(transparent-black opaque-black opaque-white)))
                       (cons 'compare (choice '#:compare compare-ops))
                       (cons 'anisotropy (and anisotropy (if (syntax-e anisotropy) 'yes 'no)))
                       (cons 'max-lod (and max-lod (syntax-e max-lod))))))
       (declare! (g-sampler (item-name) settings (stx->loc stx)) stx)]
      ['uniform
       (args! 3 "(uniform name type source)")
       (check-known-options options '() form)
       (define t (parse-type (second args)))
       (when (array-t? t)
         (zgal-error 'type-not-allowed (second args) "a uniform holds a struct, scalar, vector or matrix; use a struct holding an array"))
       (define problem (std430-problem t))
       (when problem (zgal-error 'type-not-allowed (second args) "uniform: ~a" problem))
       (define source (lookup (third args) '(per-frame) "a per-frame input"))
       (define source-type (entry-type (hash-ref env source)))
       (unless (equal? source-type t)
         (zgal-error 'type-mismatch (third args) "~a is a per-frame ~a, but the uniform holds a ~a"
                     source (type-name source-type) (type-name t)))
       (declare! (g-uniform (item-name) t source (stx->loc stx)) stx)]
      ['image
       (args! 3 "(image name format extent #:frames shared|per-frame)")
       (check-known-options options '(#:frames #:when) form)
       (define fmt (parse-format (second args)))
       (define extent (parse-extent (third args)))
       (declare! (g-image (item-name) fmt extent (frames options form stx) (condition options '#:when) (stx->loc stx)) stx)]
      ['buffer
       (args! 3 "(buffer name type count #:frames shared|per-frame)")
       (check-known-options options '(#:frames #:when) form)
       (define t (buffer-element-type (second args) form))
       ;; Computed each frame: the graph recreates the buffer when it changes
       (define count (parse-expression (third args)))
       (check-expression count env (scalar-t 'uint) "a buffer's count")
       (declare! (g-buffer (item-name) t count (frames options form stx) (condition options '#:when) (stx->loc stx)) stx)]
      ['target
       (unless (>= (length args) 2)
         (zgal-error 'malformed stx "expected (target name attachment ...), (target name (surface ...)) or (target name (if condition target target))"))
       (check-known-options options '(#:when) form)
       (define target-name (item-name))
       (define spec (parse-target-spec (rest args) stx))
       (declare! (g-target target-name spec (condition options '#:when) (stx->loc stx)) stx)]
      ['material
       (unless (>= (length args) 2)
         (zgal-error 'malformed stx "expected (material name Material (binding resource) ... #:option value ...)"))
       (declare! (parse-material-instance stx args options) stx)]
      [_ (zgal-error 'unknown-form stx "graph ~a has no ~a item; graphs take parameter, input, import-image, import-images, image-file, model, import-buffer, upload, buffer-file, constant, sampler, uniform, image, buffer, target, material, nodes (dispatch, draw-meshes, draw, readback, cpu, copy, clear, extern, reduce, inclusive-scan, exclusive-scan, sort) and loops of nodes (for)"
                     name head)]))

  ;; The type of a buffer's elements
  (define (buffer-element-type stx form)
    (define t (parse-type stx))
    (when (array-t? t)
      (zgal-error 'type-not-allowed stx "a buffer holds elements of a type; give the element type"))
    (define problem (std430-problem (array-t t 1)))
    (when problem (zgal-error 'type-not-allowed stx "~a: ~a" form problem))
    t)

  (define (parse-extent stx)
    (match (syntax->list stx)
      [(list k w h) #:when (eq? (syntax-e k) 'fixed)
       (list 'fixed (build-time w (scalar-t 'uint) "a width") (build-time h (scalar-t 'uint) "a height"))]
      [(list k scale) #:when (eq? (syntax-e k) 'surface)
       (unless (and (real? (syntax-e scale)) (> (syntax-e scale) 0))
         (zgal-error 'malformed scale "(surface scale) takes a positive number"))
       (list 'surface (syntax-e scale))]
      [(list k image) #:when (eq? (syntax-e k) 'extent-of)
       (list 'extent-of (lookup image '(import) "an imported image"))]
      [_ (zgal-error 'malformed stx "an extent is (fixed width height), (surface scale) or (extent-of imported-image), not ~a" (show stx))]))

  (define (parse-color stx)
    (match (syntax->list stx)
      [(list c ...) #:when (and (= (length c) 4) (andmap (λ (x) (real? (syntax-e x))) c))
       (map (λ (x) (exact->inexact (syntax-e x))) c)]
      [_ (zgal-error 'bad-option stx "a clear colour is (r g b a), four numbers")]))

  (define (parse-attachment stx kind)
    (define parts (syntax->list stx))
    (define-values (args options) (split-options (cdr parts) (symbol->string kind)))
    (unless (= (length args) 1)
      (zgal-error 'malformed stx "expected (~a image #:option value ...)" kind))
    (check-known-options options '(#:clear #:load #:store) (symbol->string kind))
    (define image (lookup (first args) '(image import) "an image"))
    (define clear
      (let ([c (hash-ref options '#:clear #f)])
        (cond [(not c) (if (eq? kind 'color) '(0.0 0.0 0.0 1.0) 1.0)]
              [(eq? kind 'color) (parse-color c)]
              [(real? (syntax-e c)) (exact->inexact (syntax-e c))]
              [else (zgal-error 'bad-option c "a depth clear value is a number")])))
    (t-attachment image
                  (choice-option options '#:load '(clear load dont-care) (symbol->string kind) #:default 'clear)
                  (choice-option options '#:store '(store dont-care) (symbol->string kind)
                                 #:default (if (eq? kind 'color) 'store 'dont-care))
                  clear (stx->loc stx)))

  (define (parse-target-spec stxs where)
    (match stxs
      [(list one) #:when (memq (head-symbol one) '(surface if attachments))
       (define parts (syntax->list one))
       (match (head-symbol one)
         ['surface
          (define-values (args options) (split-options (cdr parts) "surface"))
          (unless (null? args) (zgal-error 'malformed one "expected (surface #:clear (r g b a) #:depth format|none)"))
          (check-known-options options '(#:clear #:depth) "surface")
          (define depth-stx (or (hash-ref options '#:depth #f)
                                (zgal-error 'bad-option one "surface needs #:depth: a depth format (e.g. d32), or none")))
          (define depth (syntax-e depth-stx))
          (unless (or (eq? depth 'none) (depth-format? depth))
            (zgal-error 'bad-option depth-stx "surface's #:depth is a depth format (d16, d32, d24s8, d32s8) or none"))
          (t-surface (if (hash-ref options '#:clear #f) (parse-color (hash-ref options '#:clear)) '(0.0 0.0 0.0 1.0))
                     depth (stx->loc one))]
         ['if
          (match parts
            [(list _ c then else)
             (t-target-if (build-time c (scalar-t 'bool) "a target's condition")
                   (parse-target-spec (list then) then) (parse-target-spec (list else) else) (stx->loc one))]
            [_ (zgal-error 'malformed one "expected (if condition target target)")])]
         ['attachments (parse-target-spec (cdr parts) one)])]
      [_
       (define-values (colors depths)
         (partition (λ (s) (eq? (head-symbol s) 'color))
                    (for/list ([s (in-list stxs)])
                      (unless (memq (head-symbol s) '(color depth))
                        (zgal-error 'malformed s "expected (color image ...), (depth image ...), (surface ...), (attachments ...) or (if ...), found ~a" (show s)))
                      s)))
       (when (> (length depths) 1)
         (zgal-error 'malformed (second depths) "a target has one depth attachment"))
       (t-attachments (map (λ (s) (parse-attachment s 'color)) colors)
                      (and (pair? depths) (parse-attachment (first depths) 'depth))
                      (stx->loc where))]))

  (define (parse-material-instance stx args options)
    (define instance-name (parse-name (first args) "the material's name"))
    (define type-stx (second args))
    (define m (or (and (symbol? (syntax-e type-stx)) (hash-ref materials (symbol->string (syntax-e type-stx)) #f))
                  (zgal-error 'unknown-name type-stx "~a isn't a material declared earlier in the file" (show type-stx))))
    (define graphics? (eq? (material-kind m) 'graphics))
    (check-known-options options
                         (if graphics?
                             '(#:push #:cull #:front-face #:blend #:depth-test #:depth-write #:depth-compare #:topology #:when)
                             '(#:push #:invocations #:when))
                         (format "material ~a (a ~a material)" instance-name (material-kind m)))
    ;; Bindings: (binding resource), or binding alone when the resource has its name
    (define given (make-hash))
    (for ([b-stx (in-list (drop args 2))])
      (define-values (binding-stx resource-stx)
        (match (syntax->list b-stx)
          [(list b r) (values b r)]
          [#f #:when (symbol? (syntax-e b-stx)) (values b-stx b-stx)]
          [_ (zgal-error 'malformed b-stx "expected a binding (binding resource), or a binding named like its resource")]))
      (define b-name (format "~a" (syntax-e binding-stx)))
      (define b (or (for/first ([b (in-list (material-bindings m))] #:when (string=? (binding-name b) b-name)) b)
                    (zgal-error 'unknown-name binding-stx "material ~a has no binding ~a" (material-name m) b-name)))
      (when (hash-ref given b-name #f)
        (zgal-error 'duplicate-name binding-stx "binding ~a is given twice" b-name))
      (hash-set! given b-name (check-binding b resource-stx)))
    (define bindings
      (for/list ([b (in-list (material-bindings m))])
        (cons b (or (hash-ref given (binding-name b) #f)
                    (zgal-error 'incomplete-material stx "material ~a doesn't give ~a's binding ~a" instance-name
                                (material-name m) (binding-name b))))))
    ;; Push constants: those it leaves out, every node using it gives (checked once they're read)
    (define push-values (parse-push (hash-ref options '#:push #f) m))
    (define push
      (for/list ([f (in-list (material-push m))])
        (cons f (hash-ref push-values (field-name f) #f))))
    ;; Pipeline options and dispatch size
    (define (flag key)
      (define s (hash-ref options key #f))
      (cond [(not s) #f]
            [(boolean? (syntax-e s)) (if (syntax-e s) 'yes 'no)]
            [else (zgal-error 'bad-option s "~a is #t or #f" key)]))
    (define pipeline
      (if graphics?
          (filter cdr
                  (list (cons 'topology (choice-option options '#:topology '(triangle-list triangle-strip line-list line-strip point-list) "material"))
                        (cons 'cull (choice-option options '#:cull '(none back front front-and-back) "material"))
                        (cons 'front-face (choice-option options '#:front-face '(counter-clockwise clockwise) "material"))
                        (cons 'blend (choice-option options '#:blend '(opaque alpha premultiplied-alpha additive) "material"))
                        (cons 'depth-test (flag '#:depth-test))
                        (cons 'depth-write (flag '#:depth-write))
                        (cons 'depth-compare (choice-option options '#:depth-compare compare-ops "material"))))
          '()))
    ;; Optional: a material only dispatched indirectly, or by nodes giving their own, has none
    (define invocations
      (and (not graphics?)
           (hash-ref options '#:invocations #f)
           (parse-invocations (hash-ref options '#:invocations))))
    (g-material instance-name m bindings push pipeline invocations (condition options '#:when) (stx->loc stx)))

  ;; #:push ((field value) ...) for material m's push constants: a hash of the fields given to their
  ;; expressions (computed each frame)
  (define (parse-push push-stx m)
    (define push-values (make-hash))
    (when push-stx
      (for ([p-stx (in-list (or (syntax->list push-stx) (zgal-error 'malformed push-stx "#:push takes ((field value) ...)")))])
        (match (syntax->list p-stx)
          [(list f-stx value-stx)
           (define f-name (format "~a" (syntax-e f-stx)))
           (define f (or (for/first ([f (in-list (material-push m))] #:when (string=? (field-name f) f-name)) f)
                         (zgal-error 'unknown-name f-stx "material ~a has no push constant ~a" (material-name m) f-name)))
           (when (hash-ref push-values f-name #f)
             (zgal-error 'duplicate-name f-stx "push constant ~a is given twice" f-name))
           (define e (parse-expression value-stx))
           (check-expression e env (field-type f) (format "push constant ~a" f-name))
           (hash-set! push-values f-name e)]
          [_ (zgal-error 'malformed p-stx "expected (push-constant value)")])))
    push-values)

  ;; #:invocations (x [y [z]]): computed each frame, so they can follow per-frame inputs and images'
  ;; sizes
  (define (parse-invocations s)
    (define sizes (syntax->list s))
    (unless (and sizes (<= 1 (length sizes) 3))
      (zgal-error 'bad-option s "#:invocations is (x [y [z]])"))
    (for/list ([size (in-list sizes)])
      (define e (parse-expression size))
      (check-expression e env (scalar-t 'uint) "an invocation count")
      e))

  ;; The resource bound to binding b: checks it fits, and returns its name. An array binding takes
  ;; a list of resources of its elements' kind, or imported images (for arrays of images).
  (define (check-binding b stx)
    (cond
      [(not (eq? (binding-kind b) 'array)) (check-resource b stx)]
      [(syntax->list stx)
       => (λ (elements) (for/list ([e (in-list elements)]) (check-resource (binding-element b) e #:array b)))]
      [(and (symbol? (syntax-e stx))
            (match (hash-ref env (symbol->string (syntax-e stx)) #f) [(entry 'import-images _ _) #t] [_ #f]))
       (unless (memq (binding-resource-kind b) '(texture storage-image))
         (zgal-error 'binding-mismatch stx "binding ~a is an array of ~as, so it takes a list of them, but ~a is imported images"
                     (binding-name b) (binding-resource-kind b) (syntax-e stx)))
       (symbol->string (syntax-e stx))]
      [else (zgal-error 'binding-mismatch stx "binding ~a is an array, so it takes a list of resources, e.g. (~a (a b)), or imported images, not ~a"
                        (binding-name b) (binding-name b) (show stx))]))

  ;; array: the array binding b is an element of, or #f
  (define (check-resource b stx #:array [array #f])
    (define kinds
      (case (binding-kind b)
        [(texture storage-image) '(image import)]
        [(sampler) '(sampler)]
        [(buffer) '(uniform buffer import-buffer upload)]
        [(buffer-array) '(buffer import-buffer upload)]))
    (define what
      (case (binding-kind b)
        [(texture storage-image) "an image"]
        [(sampler) "a sampler"]
        [(buffer) "a uniform or buffer"]
        [(buffer-array) "a buffer"]))
    (define r (syntax-e stx))
    (define e (and (symbol? r) (hash-ref env (symbol->string r) #f)))
    (cond
      [(not (symbol? r)) (zgal-error 'malformed stx "expected a resource's name, found ~a" (show stx))]
      [(not e) (zgal-error 'unknown-name stx "~a isn't declared in graph ~a before here" r name)]
      [(not (memq (entry-kind e) kinds))
       (zgal-error 'binding-mismatch stx "binding ~a is ~a ~a, so it takes ~a, but ~a is ~a"
                   (binding-name b) (if array "an array of" "a") (binding-kind b) what r (hash-ref kind-names (entry-kind e)))])
    (when (and (eq? (binding-kind b) 'storage-image) (g-image-file? (entry-item e)))
      (zgal-error 'binding-mismatch stx "binding ~a is a storage image, but ~a is an image file, which shaders only sample"
                  (binding-name b) r))
    (when (memq (binding-kind b) '(buffer buffer-array))
      (unless (equal? (entry-type e) (binding-element b))
        (zgal-error 'binding-mismatch stx "binding ~a holds ~a, but ~a holds ~a" (binding-name b)
                    (type-name (binding-element b)) r (type-name (entry-type e))))
      (when (and (eq? (entry-kind e) 'uniform) (not (eq? (binding-access (or array b)) 'read)))
        (zgal-error 'binding-mismatch stx "binding ~a writes, but ~a is a uniform, which the CPU writes and shaders only read"
                    (binding-name b) r)))
    (symbol->string r))

  (define (parse-node stx)
    (define parts (syntax->list stx))
    (define head (syntax-e (car parts)))
    (define form (symbol->string head))
    (define-values (args options) (split-options (cdr parts) form))
    (check-known-options options
                         (case head
                           [(cpu) '(#:when #:enabled #:once #:reads #:writes)]
                           [(copy) '(#:when #:enabled #:once #:count #:source-offset #:destination-offset #:extent)]
                           [(clear) '(#:when #:enabled #:once #:value)]
                           ;; C++ adds and enables the nodes
                           [(extern) '(#:when)]
                           [(reduce inclusive-scan exclusive-scan) '(#:when #:enabled #:once #:op #:count)]
                           [(sort) '(#:when #:enabled #:once #:values #:count)]
                           [(dispatch) '(#:when #:enabled #:once #:indirect #:push #:invocations)]
                           [(draw) '(#:when #:enabled #:once #:indirect #:draws #:count #:push)]
                           [(draw-meshes) '(#:when #:enabled #:once #:indirect #:draws #:count)]
                           [else '(#:when #:enabled #:once)])
                         form)
    (define (shape n text)
      (unless (= (length args) n) (zgal-error 'malformed stx "expected ~a" text)))
    (define node-name
      (begin (when (null? args) (zgal-error 'malformed stx "a node has a name"))
             (parse-name (first args) "the node's name")))
    (when (and (pair? current-loops) (memq head '(extern reduce inclusive-scan exclusive-scan sort)))
      (zgal-error 'loop-body stx "~a ~a is in a loop, but ~a nodes aren't yet: declare it outside the loop" form node-name form))
    (node-names node-name (first args))
    (define (material-of stx kind)
      (define m-name (lookup stx '(material) "a material"))
      (define instance (entry-item (hash-ref env m-name)))
      (define m (g-material-material instance))
      (unless (eq? (material-kind m) kind)
        (zgal-error 'material-kind stx "~a needs a ~a material, but ~a is a ~a material (~a)"
                    form kind m-name (material-kind m) (material-name m)))
      (values m-name m))
    (define (node kind #:target [target #f] #:material [m #f] #:subject [subject #f] #:count [count #f]
                  #:reads [reads '()] #:writes [writes '()] #:offsets [offsets #f] #:extra [extra #f]
                  #:overrides [overrides #f])
      (g-node kind node-name target m subject count reads writes offsets
              (condition options '#:when) (condition options '#:enabled) (once?) extra overrides current-loops (stx->loc stx)))
    ;; The node's own #:push for material m, and invocations (or #f), or #f if it gives neither
    (define (overrides-of m invocations)
      (define given (parse-push (hash-ref options '#:push #f) m))
      (and (or (positive? (hash-count given)) invocations)
           (node-overrides (for/list ([f (in-list (material-push m))] #:when (hash-ref given (field-name f) #f))
                             (cons f (hash-ref given (field-name f))))
                           invocations)))
    ;; #:once #t: the node runs in the graph's first frame, then is disabled
    (define (once?)
      (define stx (hash-ref options '#:once #f))
      (cond
        [(not stx) #f]
        [(not (boolean? (syntax-e stx))) (zgal-error 'bad-option stx "#:once is #t or #f")]
        [(and (syntax-e stx) (hash-ref options '#:enabled #f))
         (zgal-error 'bad-option stx "node ~a runs once, so it has no #:enabled condition" node-name)]
        [else (syntax-e stx)]))
    ;; The imported buffers a cpu node's work reads or writes: the CPU maps buffers the application
    ;; owns (the graph's own are in device-local memory)
    (define (host-buffers key)
      (define list-stx (hash-ref options key #f))
      (cond
        [(not list-stx) '()]
        [else
         (define names (or (syntax->list list-stx)
                           (zgal-error 'malformed list-stx "~a takes a list of imported buffers, e.g. (staging)" key)))
         (for/list ([n (in-list names)])
           (define r (syntax-e n))
           (match (and (symbol? r) (hash-ref env (symbol->string r) #f))
             [(entry 'import-buffer _ _) (symbol->string r)]
             [(entry 'buffer _ _)
              (zgal-error 'type-mismatch n "~a is a graph buffer, in device-local memory the CPU can't access; a cpu node's work reads and writes imported buffers (import-buffer)" r)]
             [_ (lookup n '(import-buffer) "an imported buffer")]))]))
    (match head
      ['dispatch
       (shape 2 "(dispatch name material)")
       (define-values (m-name m) (material-of (second args) 'compute))
       (define command-stx (hash-ref options '#:indirect #f))
       (define invocations-stx (hash-ref options '#:invocations #f))
       (when (and command-stx invocations-stx)
         (zgal-error 'bad-option invocations-stx "dispatch ~a reads its workgroups from #:indirect, so it takes no #:invocations" node-name))
       (unless (or command-stx invocations-stx (g-material-invocations (entry-item (hash-ref env m-name))))
         (zgal-error 'bad-option (second args) "material ~a has no #:invocations, so dispatch ~a needs them (#:invocations on the material or the dispatch), or dispatches it #:indirect (at commands index)"
                     m-name node-name))
       (node 'dispatch #:material m-name
             #:extra (and command-stx (indirect (parse-command command-stx "DispatchCommand") #f #f #f #f))
             #:overrides (overrides-of m (and invocations-stx (parse-invocations invocations-stx))))]
      ['draw-meshes
       (shape 4 "(draw-meshes name target meshes material)")
       (define target (lookup (second args) '(target) "a target"))
       (define meshes (lookup (third args) '(meshes model) "a meshes input or a model"))
       (define-values (m-name m) (material-of (fourth args) 'graphics))
       (unless (material-vertex-input m)
         (zgal-error 'material-kind (fourth args) "draw-meshes reads vertex buffers, but material ~a (~a) has no vertex-input; draw it with (draw name target material vertex-count)"
                     m-name (material-name m)))
       (match (hash-ref env meshes)
         [(entry 'model _ model)
          (define model-format (vertex-format-name (g-model-format model)))
          (define read (vertex-format-name (vertex-input-format (material-vertex-input m))))
          (unless (equal? model-format read)
            (zgal-error 'type-mismatch (third args) "model ~a's vertices are ~a, but material ~a (~a) reads ~a"
                        meshes model-format m-name (material-name m) read))]
         [_ (void)])
       (define command-stx (hash-ref options '#:indirect #f))
       (unless command-stx
         (for ([k (in-list '(#:draws #:count))] #:when (hash-ref options k #f))
           (zgal-error 'bad-option (hash-ref options k) "~a is for draws with #:indirect" k)))
       (when (and command-stx (not (eq? (entry-kind (hash-ref env meshes)) 'model)))
         (zgal-error 'type-mismatch (third args) "draw-meshes #:indirect draws a model's mesh, but ~a is a meshes input" meshes))
       (node 'draw-meshes #:target target #:material m-name #:subject meshes
             #:extra (and command-stx (parse-indirect-draw command-stx "DrawIndexedCommand" node-name options stx)))]
      ['draw
       (define command-stx (hash-ref options '#:indirect #f))
       (if command-stx
           (shape 3 "(draw name target material #:indirect (at commands index) [#:draws n] [#:count (at counts index)])")
           (shape 4 "(draw name target material vertex-count), or (draw name target material #:indirect (at commands index))"))
       (unless command-stx
         (for ([k (in-list '(#:draws #:count))] #:when (hash-ref options k #f))
           (zgal-error 'bad-option (hash-ref options k) "~a is for draws with #:indirect" k)))
       (define target (lookup (second args) '(target) "a target"))
       (define-values (m-name m) (material-of (third args) 'graphics))
       (when (material-vertex-input m)
         (zgal-error 'material-kind (third args) "draw reads no vertex buffers, but material ~a (~a) has a vertex-input; draw it with draw-meshes"
                     m-name (material-name m)))
       (when (and command-stx (hash-ref options '#:push #f))
         (zgal-error 'bad-option (hash-ref options '#:push) "draw ~a draws #:indirect, so it takes its material's push constants: #:push is for draws of a vertex count, and dispatches" node-name))
       (if command-stx
           (node 'draw #:target target #:material m-name
                 #:extra (parse-indirect-draw command-stx "DrawCommand" node-name options stx))
           (node 'draw #:target target #:material m-name #:count (build-time (fourth args) (scalar-t 'uint) "a vertex count")
                 #:overrides (overrides-of m #f)))]
      ['readback
       (shape 2 "(readback name image-or-buffer)")
       (node 'readback #:subject (lookup (second args) '(image import buffer import-buffer upload) "an image or buffer"))]
      ['clear
       (shape 2 "(clear name buffer #:value v)")
       (define target (lookup (second args) '(buffer import-buffer upload) "a buffer"))
       (define value (hash-ref options '#:value #f))
       (node 'clear #:writes (list target)
             #:count (if value (build-time value (scalar-t 'uint) "a clear's value") (e-literal 0 (stx->loc stx))))]
      ['cpu
       (shape 2 "(cpu name input #:reads (buffer ...) #:writes (buffer ...))")
       (node 'cpu #:subject (lookup (second args) '(cpu) "a cpu input")
             #:reads (host-buffers '#:reads) #:writes (host-buffers '#:writes))]
      ['copy
       (shape 3 "(copy name source destination #:count n #:source-offset n #:destination-offset n)")
       (define source (lookup (second args) '(buffer import-buffer upload) "a buffer"))
       (define destination (lookup (third args) '(buffer import-buffer upload image import) "a buffer or image"))
       (when (equal? source destination)
         (zgal-error 'bad-copy (third args) "copy ~a copies ~a to itself; copy between two buffers" node-name source))
       (define to-image? (memq (entry-kind (hash-ref env destination)) '(image import)))
       (cond
         [to-image?
          ;; The buffer holds texels, tightly packed: a region is an offset and an extent
          (when (hash-ref options '#:count #f)
            (zgal-error 'bad-copy (hash-ref options '#:count) "copy ~a copies into an image, whose region is an #:extent (width height), not a #:count" node-name))]
         [else
          (when (hash-ref options '#:extent #f)
            (zgal-error 'bad-copy (hash-ref options '#:extent) "copy ~a copies between buffers, which take a #:count of elements, not an #:extent" node-name))
          (define source-type (entry-type (hash-ref env source)))
          (define destination-type (entry-type (hash-ref env destination)))
          (unless (equal? source-type destination-type)
            (zgal-error 'bad-copy (third args) "copy ~a copies elements, but ~a holds ~a and ~a holds ~a"
                        node-name source (type-name source-type) destination (type-name destination-type)))])
       (define (elements key what)
         (define stx (hash-ref options key #f))
         (and stx (build-time stx (scalar-t 'uint) what)))
       ;; (x y) build-time uint expressions, or #f
       (define (texels key what)
         (define stx (hash-ref options key #f))
         (and stx
              (match (syntax->list stx)
                [(list x y) (list (build-time x (scalar-t 'uint) what) (build-time y (scalar-t 'uint) what))]
                [_ (zgal-error 'bad-copy stx "copy ~a's ~a is (x y), in texels" node-name key)])))
       (define zero (e-literal 0 (stx->loc stx)))
       (define source-offset (or (elements '#:source-offset "a copy's source offset") zero))
       (if to-image?
           ;; count: the region's (width height), or #f for the rest of the image
           (node 'copy #:reads (list source) #:writes (list destination)
                 #:count (texels '#:extent "a copy's extent")
                 #:offsets (cons source-offset (or (texels '#:destination-offset "a copy's destination offset") (list zero zero))))
           (node 'copy #:reads (list source) #:writes (list destination)
                 #:count (elements '#:count "a copy's count")
                 #:offsets (cons source-offset
                                 (or (elements '#:destination-offset "a copy's destination offset") zero))))]
      ['extern
       (shape 2 "(extern name input)")
       (node 'extern #:subject (lookup (second args) '(extern) "an extern input"))]
      [(or 'reduce 'inclusive-scan 'exclusive-scan)
       (parse-kernel-node head node-name stx args options node)]
      ['sort (parse-sort-node node-name stx args options node)]))

  ;; The workgroups of the library's fixed grid for a count (see dsl/lib/kernels/reduce.zgal)
  (define (grid-groups count where)
    (define (lit n) (e-literal n where))
    (if (element-ref? count)
        (lit 256)
        (e-op 'min (list (e-op 'max (list (e-op '/ (list (e-op '+ (list count (lit 255)) where) (lit 256)) where) (lit 1)) where)
                         (lit 256))
              where)))

  ;; The frame mode of a library node's scratch buffers, from the buffers it writes: shared when
  ;; they all are, as then each frame's node waits for the previous frame's to finish writing them,
  ;; and scratch buffers per frame would only cost memory (a sort's hold as many elements as its
  ;; keys); else per frame, so frames' nodes can overlap. Imported buffers may be either.
  (define (scratch-frames written)
    (if (for/and ([b (in-list written)])
          (match (entry-item (hash-ref env b))
            [(g-buffer _ _ _ frames _ _) (eq? frames 'shared)]
            [(g-upload _ _ _ _) #t]
            [(g-buffer-file _ _ _ _) #t]
            [_ #f]))
        'shared
        'per-frame))
  ;; The most keys a sort with a count the CPU knows sorts in one workgroup (on an Apple M4, 4096
  ;; keys take 0.18 ms that way and 0.39 ms in passes; they break even near 8192)
  (define small-sort-keys 4096)

  ;; (sort name keys #:values values #:count n): sorts a buffer of uint keys in place, ascending (and
  ;; stably), moving the values with them
  (define (parse-sort-node node-name stx args options node)
    (unless (= (length args) 2)
      (zgal-error 'malformed stx "expected (sort name keys [#:values buffer] #:count n)"))
    (define where (stx->loc stx))
    (define (lit n) (e-literal n where))
    (define keys (lookup (second args) '(buffer import-buffer upload) "a buffer"))
    (unless (equal? (entry-type (hash-ref env keys)) (scalar-t 'uint))
      (zgal-error 'type-mismatch (second args) "sort's keys are uints, but ~a holds ~a (map other keys to uints that sort alike, e.g. (import sorting)'s floatSortKey)"
                  keys (type-name (entry-type (hash-ref env keys)))))
    (define values-stx (hash-ref options '#:values #f))
    (define values-buffer (and values-stx (lookup values-stx '(buffer import-buffer upload) "a buffer")))
    (define value-type (and values-buffer (entry-type (hash-ref env values-buffer))))
    (when (and value-type (not (member value-type (list (scalar-t 'uint) (scalar-t 'int) (scalar-t 'float)))))
      (zgal-error 'type-mismatch values-stx "sort's values are uints, ints or floats (e.g. indices), but ~a holds ~a" values-buffer (type-name value-type)))
    (when (equal? values-buffer keys)
      (zgal-error 'type-mismatch values-stx "sort's values are another buffer than its keys"))
    (define count (parse-kernel-count (hash-ref options '#:count #f) "sort" stx))
    (define gpu? (element-ref? count))
    ;; The scratch buffers hold as many elements as are sorted: the count, or with a count only the
    ;; GPU knows, as many as the keys' buffer holds
    (define capacity
      (cond
        [(not gpu?) count]
        [else
         (match (entry-item (hash-ref env keys))
           [(g-buffer _ _ c _ _ _) c]
           [(g-upload _ source _ _) (e-op 'count (list (e-ref source where)) where)]
           [_ (zgal-error 'type-mismatch (second args) "sort's keys ~a are ~a, whose size the graph doesn't know, so its #:count is an expression, not an element of a buffer" keys
                       (if (g-buffer-file? (entry-item (hash-ref env keys))) "read from files" "an imported buffer"))])]))
    (define groups (grid-groups count where))
    ;; With a count the CPU knows, up to small-sort-keys keys are sorted by one workgroup in one
    ;; dispatch, as the 24 dispatches of the passes take longer than sorting them
    (define small (and (not gpu?) (e-op '<= (list count (lit small-sort-keys)) where)))
    (define (unless-small e) (if small (e-op 'if (list small (lit 0) e) where) e))
    (define invocations (list (unless-small (e-op '* (list groups (lit 256)) where))))
    (define flags (append (if gpu? '(gpu-count) '()) (if values-buffer '(values) '())))
    (define suffix (format "~a~a" (if value-type (string-titlecase (type-name value-type)) "Keys") (if gpu? "Counted" "")))
    (define kernel (instantiate-kernel "sort.zgal" (hash 'T (if value-type (scalar-t-kind value-type) 'uint)) flags suffix))
    (define created-when (condition options '#:when))
    (define (internal what) (format "zgal_~a_~a" node-name what))
    (define (declare-internal! item) (declare! item stx) (set! node-items (append node-items (list item))))
    (define scratch-keys (internal "keys"))
    (define scratch-values (internal "values"))
    (define histogram (internal "histogram"))
    (define frames (scratch-frames (if values-buffer (list keys values-buffer) (list keys))))
    (declare-internal! (g-buffer scratch-keys (scalar-t 'uint) capacity frames created-when where))
    (when values-buffer
      (declare-internal! (g-buffer scratch-values value-type capacity frames created-when where)))
    (declare-internal! (g-buffer histogram (scalar-t 'uint) (lit (* 16 256)) frames created-when where))
    (define (instance! what m bindings push invocations)
      (define name (internal what))
      (declare-internal!
       (g-material name m
                   (for/list ([b (in-list (material-bindings m))]) (cons b (hash-ref bindings (binding-name b))))
                   (for/list ([f (in-list (material-push m))]) (cons f (hash-ref push (field-name f))))
                   '() invocations #f where))
      name)
    (define-values (counted-bindings counted-push)
      (if gpu?
          (values (hash "counts" (element-ref-buffer count)) (hash "countIndex" (element-ref-index count)))
          (values (hash) (hash "count" count))))
    (define (with-counts h) (for/fold ([h h]) ([(k v) (in-hash counted-bindings)]) (hash-set h k v)))
    (define (with-push h) (for/fold ([h h]) ([(k v) (in-hash counted-push)]) (hash-set h k v)))
    (define keys-bindings (hash "keys" keys "scratchKeys" scratch-keys "histogram" histogram))
    (define counter (instance! "count" (hash-ref kernel "ZgalSortCount") (with-counts keys-bindings) (with-push (hash "pass" (lit 0))) invocations))
    (define offsets (instance! "offsets" (hash-ref kernel "ZgalSortOffsets") (hash "histogram" histogram) (hash "groups" groups) (list (unless-small (lit 256)))))
    (define scatter-bindings
      (with-counts (if values-buffer
                       (hash-set* keys-bindings "items" values-buffer "scratchItems" scratch-values)
                       keys-bindings)))
    (define scatter
      (instance! "scatter" (hash-ref kernel "ZgalSortScatter") scatter-bindings
                 (with-push (hash "pass" (lit 0) "passes" (lit 1))) invocations))
    (define passes
      (append* (for/list ([pass (in-range 8)])
                 (list (k-step counter pass) (k-step offsets #f) (k-step scatter pass)))))
    (node 'kernel #:extra
          (if small
              (cons (k-step (instance! "small" (hash-ref kernel "ZgalSortScatter") scatter-bindings
                                       (with-push (hash "pass" (lit 0) "passes" (lit 8)))
                                       (list (e-op 'if (list small (lit 256) (lit 0)) where)))
                            #f)
                    passes)
              passes)))

;; The commands of (import indirect), by name: their fields, which a buffer of commands holds
  (define command-fields
    (hash "DispatchCommand" '(("x" uint) ("y" uint) ("z" uint))
          "DrawCommand" '(("vertexCount" uint) ("instanceCount" uint) ("firstVertex" uint) ("firstInstance" uint))
          "DrawIndexedCommand" '(("indexCount" uint) ("instanceCount" uint) ("firstIndex" uint) ("vertexOffset" int) ("firstInstance" uint))))

  ;; Checks t is the command struct-name of (import indirect)
  (define (check-command-type! t struct-name where what)
    (define expected (hash-ref command-fields struct-name))
    (unless (and (struct-t? t)
                 (equal? (struct-decl-name (struct-t-decl t)) struct-name)
                 (equal? (for/list ([f (in-list (struct-decl-fields (struct-t-decl t)))])
                           (list (field-name f) (type-name (field-type f))))
                         (map (λ (f) (list (first f) (symbol->string (second f)))) expected)))
      (zgal-error 'type-mismatch where "~a holds ~a" what (type-name t))))

  ;; #:indirect (at commands index): the first command, in a graph buffer of the command struct
  ;; (whose usage the graph derives, so it's an indirect buffer)
  (define (parse-command stx struct-name)
    (match (syntax->list stx)
      [(list at-stx buffer-stx index-stx) #:when (eq? (syntax-e at-stx) 'at)
       (define buffer (lookup buffer-stx '(buffer) "a graph buffer"))
       (check-command-type! (entry-type (hash-ref env buffer)) struct-name buffer-stx
                            (format "#:indirect reads ~as, (import indirect), but ~a" struct-name buffer))
       (element-ref buffer (build-time index-stx (scalar-t 'uint) "a command's index") (stx->loc stx))]
      [_ (zgal-error 'malformed stx "#:indirect is the first command's place in a buffer, (at commands index), not ~a" (show stx))]))

  ;; An indirect draw's commands: #:draws of them (default 1), or with #:count, as many as a buffer
  ;; element says. Devices without drawIndirectCount get a node copying the commands with those
  ;; past the count given no instances (declared here, used only on such devices).
  (define (parse-indirect-draw command-stx struct-name node-name options stx)
    (define command (parse-command command-stx struct-name))
    (define where (stx->loc stx))
    (define draws
      (let ([d (hash-ref options '#:draws #f)])
        (if d (build-time d (scalar-t 'uint) "#:draws") (e-literal 1 where))))
    (define count-stx (hash-ref options '#:count #f))
    (cond
      [(not count-stx) (indirect command draws #f #f #f)]
      [else
       (when (pair? current-loops)
         (zgal-error 'loop-body count-stx "draw ~a is in a loop, but draws with a #:count aren't yet: declare it outside the loop" node-name))
       (define count (parse-element count-stx "a draw's #:count"))
       (define t (entry-type (hash-ref env (element-ref-buffer command))))
       (define limit-material
         (hash-ref (instantiate-kernel "draws.zgal" (hash 'T (string->symbol struct-name)) '() (if (equal? struct-name "DrawCommand") "Draw" "DrawIndexed"))
                   "ZgalLimitDraws"))
       (define limited (format "zgal_~a_commands" node-name))
       (define limit (format "zgal_~a_limit" node-name))
       (define (declare-internal! item) (declare! item stx) (set! node-items (append node-items (list item))))
       (declare-internal! (g-buffer limited t draws 'per-frame (condition options '#:when) where))
       (declare-internal!
        (g-material limit limit-material
                    (for/list ([b (in-list (material-bindings limit-material))])
                      (cons b (case (binding-name b)
                                [("commands") (element-ref-buffer command)]
                                [("limited") limited]
                                [("counts") (element-ref-buffer count)])))
                    (for/list ([f (in-list (material-push limit-material))])
                      (cons f (case (field-name f)
                                [("first") (element-ref-index command)]
                                [("countIndex") (element-ref-index count)]
                                [("draws") draws])))
                    '() (list draws) (condition options '#:when) where))
       (indirect command draws count limit limited)]))

  ;; (at buffer index): an element of a buffer of uints (or of type), e.g. a count a shader wrote
  (define (parse-element stx what #:type [type (scalar-t 'uint)])
    (match (syntax->list stx)
      [(list at-stx buffer-stx index-stx) #:when (eq? (syntax-e at-stx) 'at)
       (define buffer (lookup buffer-stx '(buffer import-buffer upload) "a buffer"))
       (define t (entry-type (hash-ref env buffer)))
       (unless (equal? t type)
         (zgal-error 'type-mismatch buffer-stx "~a is an element of a buffer of ~as, but ~a holds ~a" what (type-name type) buffer (type-name t)))
       (element-ref buffer (build-time index-stx (scalar-t 'uint) "an element's index") (stx->loc stx))]
      [_ (zgal-error 'malformed stx "~a is a buffer's element, (at buffer index), not ~a" what (show stx))]))

  ;; A kernel's #:count: an expression, computed each frame, or an element of a buffer of uints
  ;; (which the kernel reads on the GPU)
  (define (parse-kernel-count stx form where)
    (cond
      [(not stx) (zgal-error 'bad-option where "~a needs #:count: how many elements, an expression or (at buffer index)" form)]
      [(eq? (head-symbol stx) 'at) (parse-element stx (format "~a's #:count" form))]
      [else
       (define e (parse-expression stx))
       (check-expression e env (scalar-t 'uint) (format "~a's #:count" form))
       e]))

  ;; Library algorithms: nodes dispatching materials instantiated from dsl/lib/kernels, with scratch
  ;; buffers, which this declares (named zgal_<node>_..., so they can't clash with the program's)
  (define (parse-kernel-node head node-name stx args options node)
    (define form (symbol->string head))
    (define reduce? (eq? head 'reduce))
    (unless (= (length args) 3)
      (zgal-error 'malformed stx "expected ~a"
                  (if reduce? "(reduce name source (at destination index) #:op op #:count n)"
                      (format "(~a name source destination #:op op #:count n)" form))))
    (define where (stx->loc stx))
    (define source (lookup (second args) '(buffer import-buffer upload) "a buffer"))
    (define t (entry-type (hash-ref env source)))
    (unless (member t (list (scalar-t 'uint) (scalar-t 'int) (scalar-t 'float)))
      (zgal-error 'type-mismatch (second args) "~a combines uints, ints or floats, but ~a holds ~a" form source (type-name t)))
    (define destination
      (if reduce?
          (parse-element (third args) "reduce's destination" #:type t)
          (let ([d (lookup (third args) '(buffer import-buffer upload) "a buffer")])
            (unless (equal? (entry-type (hash-ref env d)) t)
              (zgal-error 'type-mismatch (third args) "~a holds ~a, but ~a holds ~a" source (type-name t) d
                          (type-name (entry-type (hash-ref env d)))))
            d)))
    (define op (choice-option options '#:op '(add min max) form #:required? #t #:where stx))
    (define count (parse-kernel-count (hash-ref options '#:count #f) form stx))
    (define gpu? (element-ref? count))
    (define (lit n) (e-literal n where))
    ;; Workgroups: enough for the elements' tiles of 256, at most 256; with a count only the GPU
    ;; knows, all 256, which take turns over the tiles
    (define groups
      (if gpu?
          (lit 256)
          (e-op 'min (list (e-op 'max (list (e-op '/ (list (e-op '+ (list count (lit 255)) where) (lit 256)) where) (lit 1)) where)
                           (lit 256))
                where)))
    (define invocations (list (e-op '* (list groups (lit 256)) where)))
    (define (capitalized s) (string-append (string-upcase (substring s 0 1)) (substring s 1)))
    (define suffix (format "~a~a~a" (capitalized (type-name t)) (capitalized (symbol->string op)) (if gpu? "Counted" "")))
    (define flags (append (if gpu? '(gpu-count) '()) (if (eq? op 'add) '(native-scan) '())))
    (define reduce-materials (instantiate-kernel "reduce.zgal" (operator-substitutions op t) flags suffix))
    (define created-when (condition options '#:when))
    (define (internal what) (format "zgal_~a_~a" node-name what))
    (define (declare-internal! item) (declare! item stx) (set! node-items (append node-items (list item))))
    (define partials (internal "partials"))
    (declare-internal! (g-buffer partials t (lit 256) (scratch-frames (list (if reduce? (element-ref-buffer destination) destination)))
                                 created-when where))
    ;; An instance of material m. bindings, push: hashes of names to resources and expressions.
    (define (instance! what m bindings push invocations)
      (define name (internal what))
      (declare-internal!
       (g-material name m
                   (for/list ([b (in-list (material-bindings m))]) (cons b (hash-ref bindings (binding-name b))))
                   (for/list ([f (in-list (material-push m))]) (cons f (hash-ref push (field-name f))))
                   '() invocations #f where))
      name)
    (define (counted bindings push)
      (if gpu?
          (values (hash-set bindings "counts" (element-ref-buffer count)) (hash-set push "countIndex" (element-ref-index count)))
          (values bindings (hash-set push "count" count))))
    (define-values (reduce-bindings reduce-push) (counted (hash "source" source "partials" partials) (hash)))
    (define reduce-step (instance! "reduce" (hash-ref reduce-materials "ZgalScanReduce") reduce-bindings reduce-push invocations))
    (define one-group (list (lit 256)))
    (define steps
      (cond
        [reduce?
         (list reduce-step
               (instance! "finish" (hash-ref reduce-materials "ZgalReduceFinish")
                          (hash "partials" partials "destination" (element-ref-buffer destination))
                          (hash "groups" groups "index" (element-ref-index destination)) one-group))]
        [else
         (define exclusive? (eq? head 'exclusive-scan))
         (define apply-materials
           (instantiate-kernel "scan.zgal" (operator-substitutions op t) (append flags (if exclusive? '(exclusive) '()))
                               (string-append suffix (if exclusive? "Exclusive" "Inclusive"))))
         (define-values (apply-bindings apply-push)
           (counted (hash "source" source "destination" destination "partials" partials) (hash)))
         (list reduce-step
               (instance! "scanPartials" (hash-ref reduce-materials "ZgalScanPartials") (hash "partials" partials) (hash "groups" groups) one-group)
               (instance! "apply" (hash-ref apply-materials "ZgalScanApply") apply-bindings apply-push invocations))]))
    (node 'kernel #:extra (for/list ([m (in-list steps)]) (k-step m #f))))

  ;; (for (clause ...) node-or-loop ...): the nodes it declares, each in the loop (and those around it)
  (define (parse-loop stx)
    (define parts (syntax->list stx))
    (define clause-stxs (and parts (>= (length parts) 3) (syntax->list (second parts))))
    (unless (and clause-stxs (pair? clause-stxs))
      (zgal-error 'malformed stx "expected (for ([name (range from to)] ...) node ...+), whose clauses are [name (range from to [step])] or [name data-input]"))
    (define clauses
      (for/list ([c (in-list clause-stxs)])
        (define clause (parse-loop-clause c))
        (hash-set! env (loop-clause-name clause) (entry 'loop (loop-clause-type clause) clause))
        clause))
    (define outer current-loops)
    (set! current-loops (append outer (list (g-loop clauses (stx->loc stx)))))
    (define body
      (append*
       (for/list ([b (in-list (cddr parts))])
         (define head (head-symbol b))
         (cond
           [(eq? head 'for) (parse-loop b)]
           [(memq head node-forms) (list (parse-node b))]
           [else (zgal-error 'loop-body b "a loop's body holds nodes and loops, not ~a: declare it before the loop" (show b))]))))
    (set! current-loops outer)
    (for ([c (in-list clauses)]) (hash-remove! env (loop-clause-name c)))
    body)

  ;; [name (range from to [step])]: from up to, not including, to (ints, unless one is a uint); or
  ;; [name data-input]: its elements
  (define (parse-loop-clause c)
    (match (syntax->list c)
      [(list n-stx source-stx)
       (define n (parse-name n-stx "a loop variable's name"))
       (when (hash-ref env n #f)
         (zgal-error 'duplicate-name n-stx "~a is already declared in graph ~a: give the loop variable another name" n name))
       (cond
         [(eq? (head-symbol source-stx) 'range)
          (match (syntax->list source-stx)
            [(list _ from-stx to-stx step-stx ...) #:when (<= (length step-stx) 1)
             (define from (parse-expression from-stx))
             (define to (parse-expression to-stx))
             (define tf (expression-type from env #:per-frame-ok? #f))
             (define tt (expression-type to env #:per-frame-ok? #f))
             (define t (cond [(and (equal? tf literal-type) (equal? tt literal-type)) (scalar-t 'int)]
                             [(equal? tf literal-type) tt]
                             [else tf]))
             (unless (member t (list (scalar-t 'int) (scalar-t 'uint)))
               (zgal-error 'type-mismatch source-stx "a range is of ints or uints, not ~a" (a-type t)))
             (check-expression to env t "a range's end" #:per-frame-ok? #f)
             (define step
               (cond
                 [(null? step-stx) (e-literal 1 (stx->loc source-stx))]
                 [else
                  (define e (parse-expression (first step-stx)))
                  (check-expression e env t "a range's step" #:per-frame-ok? #f)
                  (when (and (e-literal? e) (<= (e-literal-value e) 0))
                    (zgal-error 'malformed (first step-stx) "a range's step is positive, not ~a" (e-literal-value e)))
                  e]))
             (loop-clause n t (list from to step) #f (stx->loc c))]
            [_ (zgal-error 'malformed source-stx "expected (range from to [step]): from up to, not including, to, by step (1 by default)")])]
         [(symbol? (syntax-e source-stx))
          (define data (lookup source-stx '(data) "a data input"))
          (loop-clause n (entry-type (hash-ref env data)) #f data (stx->loc c))]
         [else (zgal-error 'malformed source-stx "a loop goes over (range from to [step]) or a data input, not ~a" (show source-stx))])]
      [_ (zgal-error 'malformed c "expected [name (range from to [step])] or [name data-input]")]))

  (define-values (graph-items nodes)
    (for/fold ([items '()] [nodes '()] #:result (values (reverse items) (reverse nodes)))
              ([item-stx (in-list (cddr items))])
      (cond
        [(memq (head-symbol item-stx) node-forms)
         ;; A node, and the items it declares
         (let ([n (parse-node item-stx)])
           (begin0 (values (append (reverse node-items) items) (cons n nodes))
                   (set! node-items '())))]
        ;; A loop's nodes (which declare no items)
        [(eq? (head-symbol item-stx) 'for)
         (values items (append (reverse (parse-loop item-stx)) nodes))]
        [else
         ;; An item, or the items a model declares
         (let ([parsed (parse-item item-stx)])
           (values (append (reverse (if (list? parsed) parsed (list parsed))) items) nodes))])))

  ;; Every material is used, and a graphics material draws into one target
  (for ([item (in-list graph-items)] #:when (g-material? item))
    (define users (filter (λ (n) (or (equal? (g-node-material n) (g-material-name item))
                                     (and (eq? (g-node-kind n) 'kernel)
                                          (for/or ([step (in-list (g-node-extra n))]) (equal? (k-step-material step) (g-material-name item))))
                                     (and (indirect? (g-node-extra n)) (equal? (indirect-limit (g-node-extra n)) (g-material-name item)))))
                          nodes))
    (when (null? users)
      (zgal-error 'unused-material (g-material-loc item) "no node uses material ~a" (g-material-name item)))
    ;; Push constants it leaves out, every node using it gives
    (for ([pair (in-list (g-material-push item))] #:unless (cdr pair))
      (define f (car pair))
      (define gives? (λ (n) (and (g-node-overrides n)
                                 (for/or ([p (in-list (node-overrides-push (g-node-overrides n)))]) (equal? (field-name (car p)) (field-name f))))))
      (define lacking (findf (λ (n) (not (gives? n))) users))
      (when lacking
        (zgal-error 'incomplete-material (g-material-loc item)
                    "material ~a doesn't give ~a's push constant ~a: give it with #:push ((~a value)) on the material, or on every node using it (~a doesn't)"
                    (g-material-name item) (material-name (g-material-material item)) (field-name f) (field-name f) (g-node-name lacking))))
    (define targets (remove-duplicates (filter values (map g-node-target users))))
    (when (> (length targets) 1)
      (zgal-error 'material-targets (g-node-loc (second users))
                  "material ~a is drawn into targets ~a: a graphics material's pipeline is built for one target's formats, so declare a material per target"
                  (g-material-name item) (string-join targets " and "))))
  (graph name graph-items nodes (stx->loc stx)))

;; The forms of nodes
(define node-forms '(dispatch draw-meshes draw readback cpu copy clear extern reduce inclusive-scan exclusive-scan sort))
