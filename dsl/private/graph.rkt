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
         expression-type
         unify
         image-formats
         depth-format?)

;; ---------------------------------------------------------------------------------------------
;; Names in a graph

;; kind: 'parameter 'constant 'per-frame 'meshes 'cpu 'extern 'import 'import-buffer 'sampler
;; 'uniform 'image 'buffer 'target 'material. type: the value's type (parameters, constants, per-frame inputs, uniforms,
;; buffers' elements), else #f. item: the declaration.
(struct entry (kind type item) #:transparent)

(define (item-entry item)
  (match item
    [(g-parameter _ t _) (entry 'parameter t item)]
    [(g-input _ kind t _) (entry kind t item)]
    [(g-import _ _ _) (entry 'import #f item)]
    [(g-import-buffer _ t _) (entry 'import-buffer t item)]
    [(g-constant _ _ t _) (entry 'constant t item)]
    [(g-sampler _ _ _) (entry 'sampler #f item)]
    [(g-uniform _ t _ _) (entry 'uniform t item)]
    [(g-image _ _ _ _ _ _) (entry 'image #f item)]
    [(g-buffer _ t _ _ _ _) (entry 'buffer t item)]
    [(g-target _ _ _) (entry 'target #f item)]
    [(g-material _ _ _ _ _ _ _) (entry 'material #f item)]))

;; The graph's names: a hash from names to entries
(define (graph-environment g)
  (for/hash ([item (in-list (graph-items g))]) (values (graph-item-name item) (item-entry item))))

(define kind-names
  (hash 'parameter "a parameter" 'constant "a constant" 'per-frame "a per-frame input" 'meshes "a meshes input"
        'cpu "a cpu input" 'extern "an extern input" 'import "an imported image" 'import-buffer "an imported buffer" 'sampler "a sampler" 'uniform "a uniform"
        'image "an image" 'buffer "a buffer" 'target "a target" 'material "a material"))

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
       [(entry (or 'parameter 'constant) t _) t]
       [(entry 'per-frame t _)
        (unless per-frame-ok?
          (zgal-error 'not-build-time where
                      "~a is a per-frame input, but this value is computed once, when the graph is built" name))
        t]
       [(entry kind _ _)
        (zgal-error 'type-mismatch where "~a is ~a, not a value" name (hash-ref kind-names kind))])]
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
  (match e [(e-literal _ l) l] [(e-ref _ l) l] [(e-op _ _ l) l]))

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
    [(symbol? d) (e-ref (symbol->string d) (stx->loc stx))]
    [(and (syntax->list stx) (pair? (syntax->list stx)) (symbol? (syntax-e (car (syntax->list stx)))))
     (define items (syntax->list stx))
     (e-op (syntax-e (car items)) (map parse-expression (cdr items)) (stx->loc stx))]
    [else (zgal-error 'malformed stx "expected an expression, found ~a" (show stx))]))

(define (nan? x) (not (= x x)))
(define (infinite? x) (and (= x (* 2 x)) (not (zero? x))))

;; ---------------------------------------------------------------------------------------------
;; Options: keyword arguments after a form's positional ones

;; Splits a form's arguments into positional ones and a hash of keyword -> syntax
(define (split-options stxs form)
  (let loop ([stxs stxs] [positional '()] [options (hasheq)])
    (match stxs
      ['() (values (reverse positional) options)]
      [(cons k rest) #:when (keyword? (syntax-e k))
       (when (null? rest)
         (zgal-error 'bad-option k "~a needs a value" (syntax-e k)))
       (when (hash-ref options (syntax-e k) #f)
         (zgal-error 'bad-option k "~a is given twice" (syntax-e k)))
       (loop (cdr rest) positional (hash-set options (syntax-e k) (car rest)))]
      [(cons s rest)
       (unless (hash-empty? options)
         (zgal-error 'malformed s "~a: positional arguments come before keyword options" form))
       (loop rest (cons s positional) options)])))

(define (check-known-options options allowed form)
  (for ([(k stx) (in-hash options)])
    (unless (memq k allowed)
      (zgal-error 'bad-option stx "~a takes no ~a option~a" form k
                  (if (null? allowed) ""
                      (format "; it takes ~a" (string-join (map (λ (k) (format "~a" k)) allowed) ", ")))))))

;; An option whose value is one of `choices` (symbols): returns the symbol, or `default`
(define (choice-option options key choices form #:default [default #f] #:required? [required? #f] #:where [where #f])
  (define stx (hash-ref options key #f))
  (cond
    [stx
     (define v (syntax-e stx))
     (unless (memq v choices)
       (zgal-error 'bad-option stx "~a's ~a is one of ~a, not ~a" form key
                   (string-join (map symbol->string choices) ", ") (show stx)))
     v]
    [required? (zgal-error 'bad-option where "~a needs ~a (~a)" form key (string-join (map symbol->string choices) ", "))]
    [else default]))

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
;; The graph

;; stx: the (graph Name item ...) form. parse-type: parses a type. top-level: declares a top-level
;; name. materials: the file's materials so far, by name.
(define (parse-graph stx parse-type top-level materials)
  (define items (syntax->list stx))
  (unless (and items (>= (length items) 2))
    (zgal-error 'malformed stx "expected (graph Name item ...)"))
  (define name (parse-name (second items) "the graph's name"))
  (top-level name (second items))
  (define names (make-namespace (format "in graph ~a" name)))
  (define node-names (make-namespace (format "as a node of graph ~a" name)))
  (define env (make-hash))   ; name -> entry, as declared so far
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
       (args! 2 "(input name (per-frame type)), (input name meshes), (input name cpu) or (input name extern)")
       (check-known-options options '() form)
       (define kind-stx (second args))
       (define item
         (match (syntax-e kind-stx)
           ['meshes (g-input (item-name) 'meshes #f (stx->loc stx))]
           ['cpu (g-input (item-name) 'cpu #f (stx->loc stx))]
           ['extern (g-input (item-name) 'extern #f (stx->loc stx))]
           [_
            (match (syntax->list kind-stx)
              [(list k t-stx) #:when (eq? (syntax-e k) 'per-frame)
               (define t (parse-type t-stx))
               (when (array-t? t)
                 (zgal-error 'type-not-allowed t-stx "a per-frame input isn't an array; use a struct holding one"))
               (g-input (item-name) 'per-frame t (stx->loc stx))]
              [_ (zgal-error 'malformed kind-stx "an input is (per-frame type), meshes, cpu or extern, not ~a" (show kind-stx))])]))
       (declare! item stx)]
      ['import-image
       (args! 1 "(import-image name #:resting state)")
       (check-known-options options '(#:resting) form)
       (define resting (choice-option options '#:resting '(shader-read-only general transfer-src) form
                                      #:required? #t #:where stx))
       (declare! (g-import (item-name) resting (stx->loc stx)) stx)]
      ['import-buffer
       (args! 2 "(import-buffer name type)")
       (check-known-options options '() form)
       (declare! (g-import-buffer (item-name) (buffer-element-type (second args) form) (stx->loc stx)) stx)]
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
       (define count (build-time (third args) (scalar-t 'uint) "a buffer's count"))
       (declare! (g-buffer (item-name) t count (frames options form stx) (condition options '#:when) (stx->loc stx)) stx)]
      ['target
       (unless (>= (length args) 2)
         (zgal-error 'malformed stx "expected (target name attachment ...), (target name (surface ...)) or (target name (if condition target target))"))
       (check-known-options options '() form)
       (define target-name (item-name))
       (define spec (parse-target-spec (rest args) stx))
       (declare! (g-target target-name spec (stx->loc stx)) stx)]
      ['material
       (unless (>= (length args) 2)
         (zgal-error 'malformed stx "expected (material name Material (binding resource) ... #:option value ...)"))
       (declare! (parse-material-instance stx args options) stx)]
      [_ (zgal-error 'unknown-form stx "graph ~a has no ~a item; graphs take parameter, input, import-image, import-buffer, constant, sampler, uniform, image, buffer, target, material and nodes (dispatch, draw-meshes, draw, readback, cpu, copy, extern)"
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
                             '(#:push #:cull #:front-face #:blend #:depth-test #:depth-write #:depth-compare #:topology)
                             '(#:push #:invocations))
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
    ;; Push constants
    (define push-values (make-hash))
    (define push-stx (hash-ref options '#:push #f))
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
    (define push
      (for/list ([f (in-list (material-push m))])
        (cons f (or (hash-ref push-values (field-name f) #f)
                    (zgal-error 'incomplete-material stx "material ~a doesn't give ~a's push constant ~a (#:push ((~a value)))"
                                instance-name (material-name m) (field-name f) (field-name f))))))
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
    (define invocations
      (and (not graphics?)
           (let ([s (or (hash-ref options '#:invocations #f)
                        (zgal-error 'bad-option stx "compute material ~a needs #:invocations (x [y [z]]): how many invocations to dispatch"
                                    instance-name))])
             (define sizes (syntax->list s))
             (unless (and sizes (<= 1 (length sizes) 3))
               (zgal-error 'bad-option s "#:invocations is (x [y [z]])"))
             ;; Computed each frame, so they can follow per-frame inputs and images' sizes
             (for/list ([size (in-list sizes)])
               (define e (parse-expression size))
               (check-expression e env (scalar-t 'uint) "an invocation count")
               e))))
    (g-material instance-name m bindings push pipeline invocations (stx->loc stx)))

  ;; The resource bound to binding b: checks it fits, and returns its name
  (define (check-binding b stx)
    (define kinds
      (case (binding-kind b)
        [(texture storage-image) '(image import)]
        [(sampler) '(sampler)]
        [(buffer) '(uniform buffer import-buffer)]
        [(buffer-array) '(buffer import-buffer)]))
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
       (zgal-error 'binding-mismatch stx "binding ~a is a ~a, so it takes ~a, but ~a is ~a"
                   (binding-name b) (binding-kind b) what r (hash-ref kind-names (entry-kind e)))])
    (when (memq (binding-kind b) '(buffer buffer-array))
      (unless (equal? (entry-type e) (binding-element b))
        (zgal-error 'binding-mismatch stx "binding ~a holds ~a, but ~a holds ~a" (binding-name b)
                    (type-name (binding-element b)) r (type-name (entry-type e))))
      (when (and (eq? (entry-kind e) 'uniform) (not (eq? (binding-access b) 'read)))
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
                           [(cpu) '(#:when #:enabled #:reads #:writes)]
                           [(copy) '(#:when #:enabled #:count #:source-offset #:destination-offset)]
                           ;; C++ adds and enables the nodes
                           [(extern) '(#:when)]
                           [else '(#:when #:enabled)])
                         form)
    (define (shape n text)
      (unless (= (length args) n) (zgal-error 'malformed stx "expected ~a" text)))
    (define node-name
      (begin (when (null? args) (zgal-error 'malformed stx "a node has a name"))
             (parse-name (first args) "the node's name")))
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
                  #:reads [reads '()] #:writes [writes '()] #:offsets [offsets #f])
      (g-node kind node-name target m subject count reads writes offsets
              (condition options '#:when) (condition options '#:enabled) (stx->loc stx)))
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
       (define-values (m-name _m) (material-of (second args) 'compute))
       (node 'dispatch #:material m-name)]
      ['draw-meshes
       (shape 4 "(draw-meshes name target meshes material)")
       (define target (lookup (second args) '(target) "a target"))
       (define meshes (lookup (third args) '(meshes) "a meshes input"))
       (define-values (m-name m) (material-of (fourth args) 'graphics))
       (unless (material-vertex-input m)
         (zgal-error 'material-kind (fourth args) "draw-meshes reads vertex buffers, but material ~a (~a) has no vertex-input; draw it with (draw name target material vertex-count)"
                     m-name (material-name m)))
       (node 'draw-meshes #:target target #:material m-name #:subject meshes)]
      ['draw
       (shape 4 "(draw name target material vertex-count)")
       (define target (lookup (second args) '(target) "a target"))
       (define-values (m-name m) (material-of (third args) 'graphics))
       (when (material-vertex-input m)
         (zgal-error 'material-kind (third args) "draw reads no vertex buffers, but material ~a (~a) has a vertex-input; draw it with draw-meshes"
                     m-name (material-name m)))
       (node 'draw #:target target #:material m-name #:count (build-time (fourth args) (scalar-t 'uint) "a vertex count"))]
      ['readback
       (shape 2 "(readback name image)")
       (node 'readback #:subject (lookup (second args) '(image import) "an image"))]
      ['cpu
       (shape 2 "(cpu name input #:reads (buffer ...) #:writes (buffer ...))")
       (node 'cpu #:subject (lookup (second args) '(cpu) "a cpu input")
             #:reads (host-buffers '#:reads) #:writes (host-buffers '#:writes))]
      ['copy
       (shape 3 "(copy name source destination #:count n #:source-offset n #:destination-offset n)")
       (define source (lookup (second args) '(buffer import-buffer) "a buffer"))
       (define destination (lookup (third args) '(buffer import-buffer) "a buffer"))
       (when (equal? source destination)
         (zgal-error 'bad-copy (third args) "copy ~a copies ~a to itself; copy between two buffers" node-name source))
       (define source-type (entry-type (hash-ref env source)))
       (define destination-type (entry-type (hash-ref env destination)))
       (unless (equal? source-type destination-type)
         (zgal-error 'bad-copy (third args) "copy ~a copies elements, but ~a holds ~a and ~a holds ~a"
                     node-name source (type-name source-type) destination (type-name destination-type)))
       (define (elements key what)
         (define stx (hash-ref options key #f))
         (and stx (build-time stx (scalar-t 'uint) what)))
       (node 'copy #:reads (list source) #:writes (list destination)
             #:count (elements '#:count "a copy's count")
             #:offsets (cons (or (elements '#:source-offset "a copy's source offset") (e-literal 0 (stx->loc stx)))
                             (or (elements '#:destination-offset "a copy's destination offset") (e-literal 0 (stx->loc stx)))))]
      ['extern
       (shape 2 "(extern name input)")
       (node 'extern #:subject (lookup (second args) '(extern) "an extern input"))]))

  (define-values (graph-items nodes)
    (for/fold ([items '()] [nodes '()] #:result (values (reverse items) (reverse nodes)))
              ([item-stx (in-list (cddr items))])
      (if (memq (head-symbol item-stx) '(dispatch draw-meshes draw readback cpu copy extern))
          (values items (cons (parse-node item-stx) nodes))
          (values (cons (parse-item item-stx) items) nodes))))

  ;; Every material is used, and a graphics material draws into one target
  (for ([item (in-list graph-items)] #:when (g-material? item))
    (define users (filter (λ (n) (equal? (g-node-material n) (g-material-name item))) nodes))
    (when (null? users)
      (zgal-error 'unused-material (g-material-loc item) "no node uses material ~a" (g-material-name item)))
    (define targets (remove-duplicates (filter values (map g-node-target users))))
    (when (> (length targets) 1)
      (zgal-error 'material-targets (g-node-loc (second users))
                  "material ~a is drawn into targets ~a: a graphics material's pipeline is built for one target's formats, so declare a material per target"
                  (g-material-name item) (string-join targets " and "))))
  (graph name graph-items nodes (stx->loc stx)))
