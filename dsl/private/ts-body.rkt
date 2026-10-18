#lang racket/base
;; Printing lowered bodies (lower.rkt) as TypeScript: the file's functions and constants the CPU can
;; use, and apps' handlers and graph inputs. Values have C++'s semantics, through the runtime's
;; math.ts and glsl.ts: floats are rounded to 32 bits after each operation, ints and uints wrap,
;; vectors are arrays of numbers and matrices arrays of columns, and values are never changed where
;; another variable could see it (with copies the struct it changes). Floats given to generated code
;; must be 32-bit ones (Math.fround), as C++'s float parameters make them.

(require racket/list
         racket/match
         racket/set
         racket/string
         "ast.rkt"
         "body.rkt"
         "lower.rkt"
         "ts-common.rkt")

(provide ts-type
         ts-name
         ts-expression
         ts-statements
         ts-body-function
         ts-function
         ts-constant
         ts-host-value
         ts-call-target)

;; How bodies read apps' values (a function of the t-global's name, giving its TypeScript) and call
;; extern functions (a function of the name giving the callee), set by the app backend
(define ts-host-value (make-parameter (λ (name) (error 'ts-body "no host value ~a here" name))))
(define ts-call-target (make-parameter (λ (name) (ts-name name))))

;; Names JavaScript reserves, or the generated code uses, that ZGAL's names could be: spelled with a
;; zgal_ prefix (property names, e.g. struct fields, needn't be)
(define ts-reserved-words
  (list->set
   '("arguments" "await" "break" "case" "catch" "class" "const" "continue" "debugger" "default" "delete"
     "do" "else" "enum" "eval" "export" "extends" "false" "finally" "for" "function" "if" "implements"
     "import" "in" "instanceof" "interface" "let" "new" "null" "package" "private" "protected" "public"
     "return" "static" "super" "switch" "this" "throw" "true" "try" "typeof" "undefined" "var" "void"
     "while" "with" "yield" "async" "of" "NaN" "Infinity" "Math" "Number" "Object" "Array" "console"
     "zgal")))

(define (ts-name n)
  (if (set-member? ts-reserved-words n) (string-append "zgal_" n) n))

(define (ts-type t)
  (match t
    [(builder-t e) (format "~a[]" (ts-type e))]
    [_ (ts-value-type t)]))

(define (kind-of t)
  (match t
    [(scalar-t k) k]
    [(vector-t k _) k]
    [(matrix-t _) 'float]
    [_ #f]))

;; A float as JavaScript's double holding the 32-bit float nearest it
(define (f32-text v)
  (define x (floating-point-bytes->real (real->floating-point-bytes (exact->inexact v) 4)))
  (cond
    [(eqv? x +inf.0) "Infinity"]
    [(eqv? x -inf.0) "-Infinity"]
    [(eqv? x +nan.0) "NaN"]
    [(eqv? x -0.0) "-0"]
    [else
     (define text (number->string x))
     (regexp-replace #rx"\\.0$" text "")]))

(define (literal v t)
  (match t
    [(opaque-t 'string) (ts-string v)]
    [(scalar-t 'bool) (if v "true" "false")]
    [(scalar-t 'float) (f32-text v)]
    [_ (format "~a" v)]))

;; Whether e prints as a single operand
(define (atomic? e)
  (match e
    [(t-lit v (scalar-t 'float) _) (not (regexp-match? #rx"^-" (f32-text v)))]
    [(t-lit v _ _) (not (and (real? v) (negative? v)))]
    [(or (t-local _ _ _) (t-global _ _ _ _ _) (t-call _ _ _ _) (t-user-call _ _ _ _) (t-field _ _ _ _)
         (t-index _ _ _ _) (t-swizzle _ _ _ _) (t-construct _ _ _)) #t]
    [_ #f]))

(define arithmetic '(+ - * / remainder))
(define bitwise '(bitand bitor bitxor shl shr))
(define runtime-names
  (hash '+ "add" '- "sub" '* "mul" '/ "div" 'remainder "rem" 'bitand "bitand" 'bitor "bitor" 'bitxor "bitxor"
        'shl "shl" 'shr "shr"))

(define (component-index c)
  (case c [(#\x #\r #\s) 0] [(#\y #\g #\t) 1] [(#\z #\b #\p) 2] [(#\w #\a #\q) 3]))

(define (ts-expression e)
  (define p ts-expression)
  (define (operand e) (if (atomic? e) (p e) (format "(~a)" (p e))))
  (define (args es) (string-join (map p es) ", "))
  (define t (texpr-type e))
  (match e
    [(t-lit v t _) (literal v t)]
    [(t-local name _ _) (ts-name name)]
    [(t-global 'host name _ _ _) ((ts-host-value) name)]
    [(t-global _ name _ _ _) (ts-name name)]
    [(t-call f as _ _) (format "zgal.glsl.~a(~a)" f (args as))]
    [(t-user-call f as _ _) (format "~a(~a)" ((ts-call-target) f) (args as))]
    [(t-op 'negate (list a) _ _)
     (match t
       [(scalar-t 'float) (format "-~a" (operand a))]
       [(scalar-t 'int) (format "(-~a | 0)" (operand a))]
       [(scalar-t 'uint) (format "(-~a >>> 0)" (operand a))]
       [_ (format "zgal.math.neg<~a>('~a', ~a)" (ts-type t) (kind-of t) (p a))])]
    [(t-op 'not (list a) _ _) (format "!~a" (operand a))]
    [(t-op 'bitnot (list a) _ _)
     (match t
       [(scalar-t 'int) (format "~~~a" (operand a))]
       [(scalar-t 'uint) (format "(~~~a >>> 0)" (operand a))]
       [_ (format "zgal.math.bitnot<~a>('~a', ~a)" (ts-type t) (kind-of t) (p a))])]
    [(t-op 'data-count (list d) _ _) (format "~a.length" (operand d))]
    [(t-op (and op (or 'and 'or)) as _ _) (string-join (map operand as) (if (eq? op 'and) " && " " || "))]
    [(t-op (and op (or '= 'not=)) (list a b) _ _)
     (if (scalar-t? (texpr-type a))
         (format "~a ~a ~a" (operand a) (if (eq? op '=) "===" "!==") (operand b))
         (format "~azgal.math.equal(~a, ~a)" (if (eq? op '=) "" "!") (p a) (p b)))]
    [(t-op (and op (or '< '<= '> '>=)) (list a b) _ _) (format "~a ~a ~a" (operand a) op (operand b))]
    [(t-op op (list a b more ...) _ _)
     ;; Left to right, as C++ evaluates and rounds them
     (for/fold ([acc (binary op t (texpr-type a) a b)]) ([c (in-list more)])
       (binary-text op t acc (texpr-type a) (operand c)))]
    [(t-construct as (struct-t d) _)
     (format "{ ~a }" (string-join (for/list ([f (in-list (struct-decl-fields d))] [a (in-list as)])
                                     (format "~a: ~a" (field-name f) (p a)))
                                   ", "))]
    ;; collect's elements, or empty-data
    [(t-construct (list b) (data-t _) _) (p b)]
    [(t-construct '() (data-t _) _) "[]"]
    [(t-construct as (array-t _ _) _) (format "[~a]" (args as))]
    [(t-construct (list a) (scalar-t k) _) (conversion k (texpr-type a) (p a))]
    [(t-construct as (vector-t k n) _)
     (if (and (= (length as) n) (andmap (λ (a) (equal? (texpr-type a) (scalar-t k))) as))
         (format "[~a]" (args as))
         (format "zgal.math.vector<~a>('~a', ~a, ~a)" (ts-type t) k n (args as)))]
    [(t-construct as (matrix-t n) _) (format "zgal.math.matrix(~a, ~a)" n (args as))]
    [(t-field b name _ _)
     (match (texpr-type b)
       [(vector-t _ _) (format "~a[~a]" (operand b) (component-index (string-ref name 0)))]
       [_ (format "~a.~a" (operand b) name)])]
    [(t-swizzle b comps _ _)
     (if (= (string-length comps) 1)
         (format "~a[~a]" (operand b) (component-index (string-ref comps 0)))
         (format "zgal.math.swizzle(~a, [~a])" (p b)
                 (string-join (for/list ([c (in-string comps)]) (number->string (component-index c))) ", ")))]
    [(t-index b i _ _)
     (match (texpr-type b)
       [(data-t _) (format "zgal.math.elementOf(~a, ~a, 'data')" (p b) (p i))]
       [_ (format "~a[~a]" (operand b) (p i))])]
    [(t-if c a b _ _) (format "~a ? ~a : ~a" (operand c) (operand a) (operand b))]
    ;; Apps' effects, in a generated app class (ts-app.rkt), whose members they use
    [(t-effect 'print as _)
     (format "zgal.output.line([~a].join(''))"
             (string-join (for/list ([a (in-list as)])
                            (match (texpr-type a)
                              [(scalar-t 'float) (format "zgal.math.formatFloat(~a)" (p a))]
                              [(opaque-t 'string) (p a)]
                              [_ (format "String(~a)" (p a))]))
                          ", "))]
    [(t-effect 'set-frames-in-flight (list n) _) (format "this.context.setFramesInFlight(~a)" (p n))]
    [(t-effect 'resize (list w h) _) (format "this.resize = [~a, ~a]" (p w) (p h))]
    [(t-effect 'quit '() _) "this.quit = true"]
    [(t-effect 'append (list b v) _) (format "~a.push(~a)" (p b) (p v))]
    [(t-effect 'run-once (list (t-lit node _ _)) _) (format "this.runOnce_~a()" node)]))

;; a op b, where a and b are expressions of type operand-type, giving type t
(define (binary op t operand-type a b)
  (define (operand e) (if (atomic? e) (ts-expression e) (format "(~a)" (ts-expression e))))
  (binary-text op t (operand a) operand-type (operand b)))

;; The text of a op b, given the operands' texts (each a single operand)
(define (binary-text op t a operand-type b)
  (define k (kind-of t))
  (cond
    [(and (scalar-t? t) (memq op arithmetic) (memq op '(+ - *)) (eq? k 'float))
     (format "Math.fround(~a ~a ~a)" a op b)]
    [(and (scalar-t? t) (memq op '(+ -)))
     (format "(~a ~a ~a ~a)" a op b (if (eq? k 'int) "| 0" ">>> 0"))]
    [(and (scalar-t? t) (eq? op '*))
     (if (eq? k 'int) (format "Math.imul(~a, ~a)" a b) (format "(Math.imul(~a, ~a) >>> 0)" a b))]
    [(and (scalar-t? t) (eq? op '/) (eq? k 'float)) (format "Math.fround(~a / ~a)" a b)]
    [(and (scalar-t? t) (memq op bitwise))
     (define js (case op [(bitand) "&"] [(bitor) "|"] [(bitxor) "^"] [(shl) "<<"] [(shr) (if (eq? k 'uint) ">>>" ">>")]))
     (if (and (eq? k 'uint) (not (eq? op 'shr)))
         (format "((~a ~a ~a) >>> 0)" a js b)
         (format "(~a ~a ~a)" a js b))]
    [else (format "zgal.math.~a<~a>('~a', ~a, ~a)" (hash-ref runtime-names op) (ts-type t) k a b)]))

;; text, of type from, converted to scalar kind k
(define (conversion k from text)
  (define fk (kind-of from))
  (cond
    [(eq? fk k) text]
    [(and (memq k '(int uint)) (memq fk '(int uint)))
     (format "(~a ~a)" text (if (eq? k 'int) "| 0" ">>> 0"))]
    [(memq k '(float int uint bool)) (format "zgal.math.to~a(~a)" (string-titlecase (symbol->string k)) text)]
    [else (format "zgal.math.narrow('~a', ~a)" k text)]))

;; Whether e names a value another variable may hold. Lowered bodies change variables in place only
;; by assigning a struct's fields (with's copy), so a struct declared from one is copied, shallowly:
;; its fields' values are only ever replaced.
(define (place? e)
  (match e
    [(or (t-local _ _ _) (t-global _ _ _ _ _) (t-field _ _ _ _) (t-index _ _ _ _)) #t]
    [_ #f]))

;; Lines of TypeScript for statements, each indented by indent
(define (ts-statements stmts indent)
  (define p ts-expression)
  (append*
   (for/list ([s (in-list stmts)])
     (match s
       [(s-declare name (builder-t e) #f _) (list (format "~alet ~a: ~a[] = [];" indent (ts-name name) (ts-type e)))]
       [(s-declare name t #f _) (list (format "~alet ~a: ~a;" indent (ts-name name) (ts-type t)))]
       [(s-declare name t v _)
        (list (format "~alet ~a: ~a = ~a;" indent (ts-name name) (ts-type t)
                      (if (and (struct-t? t) (place? v)) (format "{ ...~a }" (p v)) (p v))))]
       [(s-assign place v _) (list (format "~a~a = ~a;" indent (p place) (p v)))]
       [(s-if c then else _)
        (define inner (string-append indent "  "))
        (append (list (format "~aif (~a) {" indent (p c)))
                (ts-statements then inner)
                (if (null? else)
                    '()
                    (cons (format "~a} else {" indent) (ts-statements else inner)))
                (list (format "~a}" indent)))]
       [(s-for name t from to step body _)
        (define n (ts-name name))
        (append (list (format "~afor (let ~a = ~a; ~a < ~a; ~a) {" indent n (p from) n (p to)
                              (match step
                                [(t-lit 1 _ _) (format "~a++" n)]
                                [_ (format "~a += ~a" n (p step))])))
                (ts-statements body (string-append indent "  "))
                (list (format "~a}" indent)))]
       [(s-effect e _) (list (format "~a~a;" indent (p e)))]
       [(s-while cs c body _)
        (define inner (string-append indent "  "))
        (append
         (if (null? cs)
             (list (format "~awhile (~a) {" indent (p c)))
             (append (list (format "~awhile (true) {" indent))
                     (ts-statements cs inner)
                     (list (format "~aif (!(~a)) break;" inner (p c)))))
         (ts-statements body inner)
         (list (format "~a}" indent)))]))))

;; A counter of temporaries' spellings for one function
(define (fresh-maker)
  (define n 0)
  (λ (base)
    (set! n (add1 n))
    (format "zgal~a~a" (string-titlecase base) n)))

;; Lines of a function or method computing e: `signature {`, e's statements, `return value; }`,
;; indented by indent. first-lines: lines its body starts with.
(define (ts-body-function signature e indent #:first-lines [first-lines '()])
  (define-values (stmts value) (lower e (fresh-maker)))
  (define inner (string-append indent "  "))
  (append (list (format "~a~a {" indent signature))
          (for/list ([l (in-list first-lines)]) (string-append inner l))
          (ts-statements stmts inner)
          (if value (list (format "~areturn ~a;" inner (ts-expression value))) '())
          (list (format "~a}" indent))))

;; A file's function, exported
(define (ts-function f)
  (ts-body-function
   (format "export function ~a(~a): ~a" (ts-name (function-decl-name f))
           (string-join (for/list ([p (in-list (function-decl-params f))]) (format "~a: ~a" (ts-name (car p)) (ts-type (cdr p)))) ", ")
           (ts-type (function-decl-result f)))
   (function-decl-body f) ""))

;; A file's constant, exported; one needing statements computes them in a function called once
(define (ts-constant c)
  (define-values (stmts value) (lower (constant-decl-body c) (fresh-maker)))
  (define name (ts-name (constant-decl-name c)))
  (define t (ts-type (constant-decl-type c)))
  (if (null? stmts)
      (list (format "export const ~a: ~a = ~a;" name t (ts-expression value)))
      (append (list (format "export const ~a: ~a = (() => {" name t))
              (ts-statements stmts "  ")
              (list (format "  return ~a;" (ts-expression value)) "})();"))))
