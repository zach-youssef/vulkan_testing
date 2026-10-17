#lang racket/base
;; Memory layouts. Buffers and push constants use std430, which the generated C++ structs must
;; match byte for byte; vertex attributes are read from C++ structs laid out by the C++ compiler.
;;
;; The C++ structs get the std430 offsets by placing every member explicitly: GLM's types are
;; 4-byte aligned (unless GLM_FORCE_DEFAULT_ALIGNED_GENTYPES is defined), so the compiler adds no
;; padding of its own and the generated padding members do the rest. Types whose std430 layout
;; can't be expressed that way (see std430-problem) are rejected.

(require racket/list
         racket/match
         "ast.rkt")

(provide std430-align
         std430-size
         std430-stride
         std430-problem
         interface-problem
         cpp-size
         (struct-out placed)
         place-fields
         round-up
         struct-size)

(define (round-up n alignment)
  (* alignment (quotient (+ n alignment -1) alignment)))

(define (std430-align t)
  (match t
    [(scalar-t _) 4]
    [(vector-t _ 2) 8]
    [(vector-t _ _) 16]
    [(matrix-t n) (std430-align (vector-t 'float n))]   ; an array of columns
    [(array-t e _) (std430-align e)]
    [(struct-t d) (struct-align d)]))

(define (std430-size t)
  (match t
    [(scalar-t _) 4]
    [(vector-t _ n) (* 4 n)]
    [(matrix-t n) (* n (std430-stride (vector-t 'float n)))]
    [(array-t e n) (* n (std430-stride e))]
    [(struct-t d) (struct-size d)]))

;; The distance between consecutive array elements
(define (std430-stride t)
  (round-up (std430-size t) (std430-align t)))

(define (struct-align d)
  (apply max 4 (for/list ([f (in-list (struct-decl-fields d))]) (std430-align (field-type f)))))

;; A struct's size is its last member's end, rounded up to the struct's alignment
(define struct-sizes (make-weak-hasheq))
(define (struct-size d)
  (hash-ref! struct-sizes d
             (λ ()
               (define-values (_ end) (place-fields (struct-decl-fields d) 0))
               (round-up end (struct-align d)))))

;; The size of the C++ type the generated code uses for t
(define (cpp-size t)
  (match t
    [(scalar-t _) 4]
    [(vector-t _ n) (* 4 n)]
    [(matrix-t n) (* 4 n n)]
    [(array-t e n) (* n (cpp-size e))]
    [(struct-t d) (struct-size d)]))

;; A field and its std430 offset
(struct placed (field offset) #:transparent)

;; Places fields one after another from `start`: returns the placed fields and the end offset
(define (place-fields fields start)
  (for/fold ([acc '()] [offset start] #:result (values (reverse acc) offset))
            ([f (in-list fields)])
    (define t (field-type f))
    (define at (round-up offset (std430-align t)))
    (values (cons (placed f at) acc) (+ at (std430-size t)))))

;; Why t can't be used in a buffer or push constants, or #f if it can
(define (std430-problem t)
  (match t
    [(scalar-t 'bool) "bool has no fixed size in buffers and push constants; use uint"]
    [(vector-t 'bool _) "boolean vectors have no fixed size in buffers and push constants; use uvec"]
    [(matrix-t 3) "std430 pads each column of a mat3 to 16 bytes, but GLM's glm::mat3 doesn't; use mat4"]
    [(array-t (array-t _ _) _) "arrays of arrays aren't supported; use a struct holding an array"]
    [(array-t e _)
     (or (std430-problem e)
         (and (not (= (cpp-size e) (std430-stride e)))
              (format "std430 pads each ~a in an array to ~a bytes, but C++ doesn't; use a 4-component vector"
                      (type-name e) (std430-stride e))))]
    [_ #f]))

;; Why t can't be a vertex attribute, varying or fragment output, or #f if it can
(define (interface-problem t)
  (match t
    [(or (scalar-t (or 'float 'int 'uint)) (vector-t (or 'float 'int 'uint) _)) #f]
    [_ (format "~a isn't a scalar or vector of float, int or uint" (type-name t))]))
