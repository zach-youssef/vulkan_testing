#lang racket/base
;; C++ spellings shared by the C++ backend's modules

(require racket/match
         "ast.rkt")

(provide cpp-string
         scalar-cpp
         cpp-type
         cpp-declaration)

;; A C++ string literal
(define (cpp-string s)
  (string-append "\"" (regexp-replace* #px"[\\\\\"]" s "\\\\&") "\""))

(define (scalar-cpp kind)
  (case kind [(float) "float"] [(int) "int32_t"] [(uint) "uint32_t"] [(bool) "uint32_t"]))

(define (cpp-type t)
  (match t
    [(scalar-t k) (scalar-cpp k)]
    [(vector-t k n) (format "glm::~a" (type-name t))]
    [(matrix-t n) (format "glm::mat~a" n)]
    [(struct-t d) (struct-decl-name d)]))

(define (cpp-declaration t name)
  (match t
    [(array-t e n) (format "~a ~a[~a]" (cpp-type e) name n)]
    [_ (format "~a ~a" (cpp-type t) name)]))
