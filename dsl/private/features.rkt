#lang racket/base
;; What a program needs of the device beyond what the wrapper requires: 8- and 16-bit integers.
;; The GLSL backend enables their extensions, and the C++ backend reports the device features each
;; material (and graph) needs.

(require racket/list
         racket/match
         "ast.rkt"
         "body.rkt"
         "layout.rkt")

(provide program-integer-widths
         material-integer-widths)

;; The types of e and everything in it, including locals and accumulators
(define (texpr-types e)
  (append
   (let ([t (texpr-type e)]) (if (or (void-t? t) (results-t? t) (values-t? t)) '() (list t)))
   (match e
     [(t-let bindings _ _ _) (append-map second bindings)]
     [(t-fold accs _ _ _ _) (map second accs)]
     [_ '()])
   (append-map texpr-types (texpr-children e))))

(define (widths-of types)
  (sort (remove-duplicates
         (for*/list ([t (in-list types)] [k (in-list (small-integer-kinds-in t))])
           (small-integer-bits k)))
        <))

(define (declaration-types d)
  (match d
    [(function-decl _ params result body _ _ _) (append (map cdr params) (list result) (texpr-types body))]
    [(constant-decl _ type body _ _ _) (cons type (texpr-types body))]))

(define (declarations-by-name prog)
  (for/hash ([d (in-list (program-functions prog))])
    (values (if (function-decl? d) (function-decl-name d) (constant-decl-name d)) d)))

;; The integer widths (8, 16) material m's shaders use: its buffers' and shared variables' types,
;; and what its ZGAL bodies (and the functions they use) compute with. GLSL bodies can only be
;; judged by their bindings and shared variables.
(define (material-integer-widths m prog)
  (define declarations (declarations-by-name prog))
  (widths-of
   (append
    (for/list ([b (in-list (material-bindings m))] #:when (binding-element b)) (binding-element b))
    (map field-type (material-shared m))
    (for*/list ([s (in-list (material-shaders m))] #:when (zgal-shader? s)
                [t (in-list (append (texpr-types (zgal-shader-body s))
                                    (append-map (λ (n) (declaration-types (hash-ref declarations n)))
                                                (zgal-shader-uses s))))])
      t))))

;; The integer widths used anywhere in prog, which every shader enables the extensions for: its
;; shaders declare all of its structs
(define (program-integer-widths prog)
  (widths-of
   (append
    (for*/list ([d (in-list (program-structs prog))] [f (in-list (struct-decl-fields d))]) (field-type f))
    (append-map declaration-types (program-functions prog))
    (for*/list ([m (in-list (program-materials prog))]
                [w (in-list (material-integer-widths m prog))])
      (scalar-t (if (= w 8) 'uint8 'uint16))))))
