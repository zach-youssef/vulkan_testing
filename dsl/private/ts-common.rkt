#lang racket/base
;; TypeScript spellings shared by the TypeScript backend's modules

(require racket/match
         racket/string
         "ast.rkt")

(provide ts-string
         ts-value-type
         ts-layout-type)

;; A TypeScript string literal
(define (ts-string s)
  (string-append "'" (regexp-replace* #px"[\\\\'\n]" s (λ (c) (if (string=? c "\n") "\\n" (string-append "\\" c)))) "'"))

;; The TypeScript type of values of type t: numbers, arrays of numbers (vectors), arrays of columns
;; (matrices), arrays, and the structs' interfaces
(define (ts-value-type t)
  (match t
    [(scalar-t 'bool) "boolean"]
    [(scalar-t _) "number"]
    [(vector-t 'bool _) "boolean[]"]
    [(vector-t _ _) "number[]"]
    [(matrix-t _) "number[][]"]
    [(array-t e _) (format "~a[]" (ts-value-type e))]
    [(struct-t d) (struct-decl-name d)]
    [(data-t e) (format "readonly ~a[]" (ts-value-type e))]
    [(opaque-t 'path) "string"]
    [(void-t) "void"]))

;; The runtime's description of type t's layout (web/zgal/types.ts)
(define (ts-layout-type t)
  (match t
    [(scalar-t k) (format "zgal.~a" k)]
    [(vector-t 'bool n) (format "zgal.vector(zgal.uint, ~a)" n)]
    [(vector-t _ _) (format "zgal.~a" (type-name t))]
    [(matrix-t n) (format "zgal.mat~a" n)]
    [(array-t e n) (format "zgal.array(~a, ~a)" (ts-layout-type e) n)]
    [(struct-t d) (struct-decl-name d)]))
