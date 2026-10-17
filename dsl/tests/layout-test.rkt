#lang racket/base
;; std430 layouts, checked against the GLSL specification's rules (OpenGL 4.6, 7.6.2.2)

(require racket/list
         rackunit
         "../private/ast.rkt"
         "../private/layout.rkt")

(define float (scalar-t 'float))
(define uint (scalar-t 'uint))
(define (vec n) (vector-t 'float n))
(define (decl name . fields)
  (struct-decl name (for/list ([f (in-list fields)]) (field (car f) (cdr f) #f)) #f))
(define (offsets d)
  (define-values (placed _end) (place-fields (struct-decl-fields d) 0))
  (map placed-offset placed))

(test-case "Scalars and vectors align to their size, except 3-component vectors to 16"
  (check-equal? (map std430-align (list float (vec 2) (vec 3) (vec 4))) '(4 8 16 16))
  (check-equal? (map std430-size (list float (vec 2) (vec 3) (vec 4))) '(4 8 12 16)))

(test-case "Matrices are arrays of their columns"
  (check-equal? (list (std430-align (matrix-t 2)) (std430-size (matrix-t 2))) '(8 16))
  (check-equal? (list (std430-align (matrix-t 3)) (std430-size (matrix-t 3))) '(16 48))
  (check-equal? (list (std430-align (matrix-t 4)) (std430-size (matrix-t 4))) '(16 64)))

(test-case "std430 arrays aren't padded to 16 bytes, as std140's are"
  (check-equal? (std430-stride float) 4)
  (check-equal? (std430-size (array-t float 3)) 12)
  (check-equal? (std430-size (array-t (vec 2) 3)) 24)
  (check-equal? (std430-stride (vec 3)) 16))

(test-case "A scalar packs into the space after a vec3"
  (check-equal? (offsets (decl "A" (cons "a" (vec 3)) (cons "b" float))) '(0 12))
  (check-equal? (std430-size (struct-t (decl "A" (cons "a" (vec 3)) (cons "b" float)))) 16)
  ;; But a vec2 doesn't fit
  (check-equal? (offsets (decl "B" (cons "a" (vec 3)) (cons "b" (vec 2)))) '(0 16)))

(test-case "Structs align to their most aligned member and round their size up to it"
  (define inner (decl "Inner" (cons "a" (vec 2)) (cons "b" float)))
  (check-equal? (std430-align (struct-t inner)) 8)
  (check-equal? (std430-size (struct-t inner)) 16)
  (define outer (decl "Outer" (cons "s" float) (cons "inner" (struct-t inner)) (cons "t" float)))
  (check-equal? (offsets outer) '(0 8 24))
  (check-equal? (std430-size (struct-t outer)) 32)
  (check-equal? (std430-stride (struct-t inner)) 16)
  (check-equal? (std430-size (array-t (struct-t inner) 2)) 32))

(test-case "The probe struct of the GPU tests has the offsets its comments give"
  (define inner (decl "Inner" (cons "a" (vec 2)) (cons "b" float)))
  (define tricky
    (decl "Tricky" (cons "s" float) (cons "v3" (vec 3)) (cons "f" float) (cons "v2" (vec 2))
          (cons "i" (scalar-t 'int)) (cons "u" (vector-t 'uint 3)) (cons "m2" (matrix-t 2))
          (cons "inner" (struct-t inner)) (cons "scalars" (array-t float 3))
          (cons "inners" (array-t (struct-t inner) 2)) (cons "m4" (matrix-t 4)) (cons "last" uint)))
  (check-equal? (offsets tricky) '(0 16 28 32 40 48 64 80 96 112 144 208))
  (check-equal? (std430-size (struct-t tricky)) 224))

(test-case "Push fields are placed after the binding indices"
  (define-values (placed end) (place-fields (list (field "a" (vec 3) #f) (field "b" uint #f)) 12))
  (check-equal? (map placed-offset placed) '(16 28))
  (check-equal? end 32))

(test-case "Types C++ can't lay out like std430 are rejected, with the reason"
  (check-false (std430-problem (array-t (vec 4) 2)))
  (check-false (std430-problem (array-t (matrix-t 2) 2)))
  (check-regexp-match #rx"use uint" (std430-problem (scalar-t 'bool)))
  (check-regexp-match #rx"use mat4" (std430-problem (matrix-t 3)))
  (check-regexp-match #rx"4-component" (std430-problem (array-t (vec 3) 2)))
  (check-regexp-match #rx"arrays of arrays" (std430-problem (array-t (array-t float 2) 2))))

(test-case "Vertex attributes, varyings and outputs are scalars or vectors of float, int or uint"
  (check-false (interface-problem (vector-t 'uint 2)))
  (check-false (interface-problem float))
  (check-true (string? (interface-problem (matrix-t 4))))
  (check-true (string? (interface-problem (scalar-t 'bool)))))
