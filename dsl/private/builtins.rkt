#lang racket/base
;; Shader bodies' built-in functions and variables: their signatures, and which overload a call
;; resolves to. Names and semantics are GLSL's; the backends spell them.

(require racket/list
         racket/match
         "ast.rkt")

(provide literal-type
         literal?
         numeric-kind?
         component-kind
         resolve-builtin
         builtin-function?
         builtin-names
         stage-variables
         stage-variable-names)

;; Integer literals take the type their context needs, so they have a type of their own until then
(define literal-type (scalar-t 'literal))
(define (literal? t) (equal? t literal-type))

(define (numeric-kind? k) (and (memq k '(float int uint)) #t))

;; The scalar kind of a scalar or vector type, or #f
(define (component-kind t)
  (match t
    [(scalar-t k) k]
    [(vector-t k _) k]
    [_ #f]))

;; ---------------------------------------------------------------------------------------------
;; Signatures. An argument or result pattern is
;;   'genF 'genI 'genU 'genB  a type T over float, int, uint or bool scalars and vectors, the same T
;;                            throughout the signature
;;   'vecF 'vecI 'vecU 'vecB  likewise, vectors only
;;   'scalarT                 T's scalar type
;;   'bvecT                   a bool vector the size of T
;;   'mat                     a matrix type M, the same throughout
;;   a type                   exactly that type

(define (gen kind) (case kind [(F) 'genF] [(I) 'genI] [(U) 'genU] [(B) 'genB]))

(define (unary names kinds)
  (for*/list ([n (in-list names)]) (cons n (for/list ([k (in-list kinds)]) (list (list (gen k)) (gen k))))))

(define float-t (scalar-t 'float))
(define int-t (scalar-t 'int))
(define uint-t (scalar-t 'uint))
(define bool-t (scalar-t 'bool))
(define vec3-t (vector-t 'float 3))

(define signatures
  (make-immutable-hasheq
   (append
    (unary '(radians degrees sin cos tan asin acos sinh cosh tanh asinh acosh atanh exp log exp2 log2
             sqrt inversesqrt floor trunc round roundEven ceil fract normalize)
           '(F))
    (unary '(abs sign) '(F I))
    (list
     (cons 'atan '(((genF) genF) ((genF genF) genF)))
     (cons 'pow '(((genF genF) genF)))
     (cons 'mod '(((genF genF) genF) ((genF scalarT) genF)))
     (cons 'min '(((genF genF) genF) ((genF scalarT) genF) ((genI genI) genI) ((genI scalarT) genI)
                  ((genU genU) genU) ((genU scalarT) genU)))
     (cons 'max '(((genF genF) genF) ((genF scalarT) genF) ((genI genI) genI) ((genI scalarT) genI)
                  ((genU genU) genU) ((genU scalarT) genU)))
     (cons 'clamp '(((genF genF genF) genF) ((genF scalarT scalarT) genF) ((genI genI genI) genI)
                    ((genI scalarT scalarT) genI) ((genU genU genU) genU) ((genU scalarT scalarT) genU)))
     (cons 'mix '(((genF genF genF) genF) ((genF genF scalarT) genF) ((genF genF bvecT) genF)))
     (cons 'step '(((genF genF) genF) ((scalarT genF) genF)))
     (cons 'smoothstep '(((genF genF genF) genF) ((scalarT scalarT genF) genF)))
     (cons 'length `(((genF) ,float-t)))
     (cons 'distance `(((genF genF) ,float-t)))
     (cons 'dot `(((genF genF) ,float-t)))
     (cons 'cross `(((,vec3-t ,vec3-t) ,vec3-t)))
     (cons 'reflect '(((genF genF) genF)))
     (cons 'refract `(((genF genF ,float-t) genF)))
     (cons 'faceforward '(((genF genF genF) genF)))
     (cons 'lessThan '(((vecF vecF) bvecT) ((vecI vecI) bvecT) ((vecU vecU) bvecT)))
     (cons 'lessThanEqual '(((vecF vecF) bvecT) ((vecI vecI) bvecT) ((vecU vecU) bvecT)))
     (cons 'greaterThan '(((vecF vecF) bvecT) ((vecI vecI) bvecT) ((vecU vecU) bvecT)))
     (cons 'greaterThanEqual '(((vecF vecF) bvecT) ((vecI vecI) bvecT) ((vecU vecU) bvecT)))
     (cons 'equal '(((vecF vecF) bvecT) ((vecI vecI) bvecT) ((vecU vecU) bvecT) ((vecB vecB) bvecT)))
     (cons 'notEqual '(((vecF vecF) bvecT) ((vecI vecI) bvecT) ((vecU vecU) bvecT) ((vecB vecB) bvecT)))
     (cons 'any `(((vecB) ,bool-t)))
     (cons 'all `(((vecB) ,bool-t)))
     (cons 'determinant `(((mat) ,float-t)))
     (cons 'inverse '(((mat) mat)))
     (cons 'transpose '(((mat) mat)))
     (cons 'floatBitsToUint '(((genF) genU)))
     (cons 'floatBitsToInt '(((genF) genI)))
     (cons 'uintBitsToFloat '(((genU) genF)))
     (cons 'intBitsToFloat '(((genI) genF)))))))

(define (builtin-function? name) (hash-has-key? signatures name))

(define (builtin-names) (sort (hash-keys signatures) symbol<?))

;; The kind a pattern's type variable ranges over, and whether it's vectors only
(define (pattern-kind p)
  (case p
    [(genF vecF) 'float] [(genI vecI) 'int] [(genU vecU) 'uint] [(genB vecB) 'bool] [else #f]))
(define (vector-pattern? p) (and (memq p '(vecF vecI vecU vecB)) #t))

;; Resolves a call: returns (cons argument-types result-type), with literal arguments given the
;; types they take, or #f if no overload matches
(define (resolve-builtin name arg-types)
  (for/or ([signature (in-list (hash-ref signatures name '()))])
    (match-define (list patterns result) signature)
    (and (= (length patterns) (length arg-types))
         (match-signature patterns result arg-types))))

(define (match-signature patterns result arg-types)
  ;; Bind T and M from the non-literal arguments first, then check everything
  (define T #f)
  (define M #f)
  (define (bind! p t)
    (cond
      [(literal? t) #t]
      [(pattern-kind p)
       (and (equal? (component-kind t) (pattern-kind p))
            (or (not (vector-pattern? p)) (vector-t? t))
            (cond [T (equal? T t)] [else (set! T t) #t]))]
      [(eq? p 'mat) (and (matrix-t? t) (cond [M (equal? M t)] [else (set! M t) #t]))]
      [else #t]))
  (define (expected p)
    (cond
      [(pattern-kind p) T]
      [(eq? p 'scalarT) (and T (scalar-t (component-kind T)))]
      [(eq? p 'bvecT) (and (vector-t? T) (vector-t 'bool (vector-t-size T)))]
      [(eq? p 'mat) M]
      [else p]))
  (and (for/and ([p (in-list patterns)] [t (in-list arg-types)]) (bind! p t))
       ;; A literal standing for T, with T unbound, makes T its kind's scalar
       (or T
           (for/first ([p (in-list patterns)] [t (in-list arg-types)] #:when (and (literal? t) (pattern-kind p)))
             (set! T (scalar-t (pattern-kind p)))
             #t)
           #t)
       (let ([resolved (for/list ([p (in-list patterns)] [t (in-list arg-types)])
                         (define e (expected p))
                         (cond
                           [(not e) #f]
                           [(literal? t) (and (scalar-t? e) (numeric-kind? (scalar-t-kind e)) e)]
                           [(equal? t e) e]
                           [else #f]))])
         (and (andmap values resolved)
              (let ([r (expected result)])
                (and r (cons resolved r)))))))

;; ---------------------------------------------------------------------------------------------
;; Stage variables: ZGAL name -> (list GLSL name, type)

(define stage-variables
  (hash 'vertex (hash "vertexIndex" (list "gl_VertexIndex" int-t)
                      "instanceIndex" (list "gl_InstanceIndex" int-t))
        'fragment (hash "fragCoord" (list "gl_FragCoord" (vector-t 'float 4))
                        "frontFacing" (list "gl_FrontFacing" bool-t))
        'compute (hash "globalInvocationId" (list "gl_GlobalInvocationID" (vector-t 'uint 3))
                       "localInvocationId" (list "gl_LocalInvocationID" (vector-t 'uint 3))
                       "workgroupId" (list "gl_WorkGroupID" (vector-t 'uint 3))
                       "workgroupCount" (list "gl_NumWorkGroups" (vector-t 'uint 3)))))

(define stage-variable-names
  (for*/list ([(stage vars) (in-hash stage-variables)] [name (in-hash-keys vars)]) name))
